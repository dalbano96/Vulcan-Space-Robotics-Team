#include "IHIDDevice.hpp"
#include "DeviceToken.hpp"
#include "DeviceBase.hpp"
#include <thread>
#include <mutex>
#include <condition_variable>

#include <stdio.h>
#include <libudev.h>
#include <stropts.h>
#include <linux/usb/ch9.h>
#include <linux/usbdevice_fs.h>
#include <linux/input.h>
#include <linux/hidraw.h>
#include <linux/hiddev.h>
#include <linux/joystick.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <string.h>

namespace boo
{

extern const char* UDevButtonNames[];
extern const char* UDevAxisNames[];
udev* GetUdev();
udev_device* GetUdevJoystick(udev_device* parent);
udev_device* GetUdevHidRaw(udev_device* parent);

/*
 * Reference: http://tali.admingilde.org/linux-docbook/usb/ch07s06.html
 */

class HIDDeviceUdev final : public IHIDDevice
{
    DeviceToken& m_token;
    DeviceBase& m_devImp;

    int m_rawFd = 0;
    int m_joyFd = 0;
    unsigned m_usbIntfInPipe = 0;
    unsigned m_usbIntfOutPipe = 0;
    bool m_runningTransferLoop = false;

    const std::string& m_devPath;
    std::mutex m_initMutex;
    std::condition_variable m_initCond;
    std::thread m_thread;

    bool _sendUSBInterruptTransfer(const uint8_t* data, size_t length)
    {
        if (m_rawFd)
        {
            usbdevfs_bulktransfer xfer =
            {
                m_usbIntfOutPipe | USB_DIR_OUT,
                (unsigned)length,
                30,
                (void*)data
            };
            int ret = ioctl(m_rawFd, USBDEVFS_BULK, &xfer);
            if (ret != (int)length)
                return false;
            return true;
        }
        return false;
    }

    size_t _receiveUSBInterruptTransfer(uint8_t* data, size_t length)
    {
        if (m_rawFd)
        {
            usbdevfs_bulktransfer xfer =
            {
                m_usbIntfInPipe | USB_DIR_IN,
                (unsigned)length,
                30,
                data
            };
            return ioctl(m_rawFd, USBDEVFS_BULK, &xfer);
        }
        return 0;
    }

    static void _threadProcUSBLL(HIDDeviceUdev* device)
    {
        int i;
        char errStr[256];
        std::unique_lock<std::mutex> lk(device->m_initMutex);
        udev_device* udevDev = udev_device_new_from_syspath(GetUdev(), device->m_devPath.c_str());

        /* Get device file */
        const char* dp = udev_device_get_devnode(udevDev);
        int fd = open(dp, O_RDWR);
        if (fd < 0)
        {
            snprintf(errStr, 256, "Unable to open %s@%s: %s\n",
                     device->m_token.getProductName().c_str(), dp, strerror(errno));
            device->m_devImp.deviceError(errStr);
            lk.unlock();
            device->m_initCond.notify_one();
            udev_device_unref(udevDev);
            return;
        }
        device->m_rawFd = fd;
        usb_device_descriptor devDesc = {};
        read(fd, &devDesc, 1);
        read(fd, &devDesc.bDescriptorType, devDesc.bLength-1);
        if (devDesc.bNumConfigurations)
        {
            usb_config_descriptor confDesc = {};
            read(fd, &confDesc, 1);
            read(fd, &confDesc.bDescriptorType, confDesc.bLength-1);
            if (confDesc.bNumInterfaces)
            {
                usb_interface_descriptor intfDesc = {};
                read(fd, &intfDesc, 1);
                read(fd, &intfDesc.bDescriptorType, intfDesc.bLength-1);
                for (i=0 ; i<intfDesc.bNumEndpoints+1 ; ++i)
                {
                    usb_endpoint_descriptor endpDesc = {};
                    read(fd, &endpDesc, 1);
                    read(fd, &endpDesc.bDescriptorType, endpDesc.bLength-1);
                    if ((endpDesc.bmAttributes & USB_ENDPOINT_XFERTYPE_MASK) == USB_ENDPOINT_XFER_INT)
                    {
                        if ((endpDesc.bEndpointAddress & USB_ENDPOINT_DIR_MASK) == USB_DIR_IN)
                            device->m_usbIntfInPipe = endpDesc.bEndpointAddress & USB_ENDPOINT_NUMBER_MASK;
                        else if ((endpDesc.bEndpointAddress & USB_ENDPOINT_DIR_MASK) == USB_DIR_OUT)
                            device->m_usbIntfOutPipe = endpDesc.bEndpointAddress & USB_ENDPOINT_NUMBER_MASK;
                    }
                }
            }
        }

        /* Request that kernel disconnects existing driver */
        usbdevfs_ioctl disconnectReq = {
            0,
            USBDEVFS_DISCONNECT,
            NULL
        };
        ioctl(fd, USBDEVFS_IOCTL, &disconnectReq);

        /* Return control to main thread */
        device->m_runningTransferLoop = true;
        lk.unlock();
        device->m_initCond.notify_one();

        /* Start transfer loop */
        device->m_devImp.initialCycle();
        while (device->m_runningTransferLoop)
            device->m_devImp.transferCycle();
        device->m_devImp.finalCycle();

        /* Cleanup */
        close(fd);
        device->m_rawFd = 0;
        udev_device_unref(udevDev);
    }

    static void _threadProcBTLL(HIDDeviceUdev* device)
    {
        std::unique_lock<std::mutex> lk(device->m_initMutex);
        udev_device* udevDev = udev_device_new_from_syspath(GetUdev(), device->m_devPath.c_str());

        /* Return control to main thread */
        device->m_runningTransferLoop = true;
        lk.unlock();
        device->m_initCond.notify_one();

        /* Start transfer loop */
        device->m_devImp.initialCycle();
        while (device->m_runningTransferLoop)
            device->m_devImp.transferCycle();
        device->m_devImp.finalCycle();

        udev_device_unref(udevDev);
    }

    static void _threadProcHID(HIDDeviceUdev* device)
    {
        char errStr[256];
        std::unique_lock<std::mutex> lk(device->m_initMutex);
        udev_device* udevDev = udev_device_new_from_syspath(GetUdev(), device->m_devPath.c_str());

        udev_device* rawDev = GetUdevHidRaw(udevDev);
        udev_device* jsDev = GetUdevJoystick(udevDev);

        /* Get raw file */
        int rfd = 0;
        if (rawDev)
        {
            const char* rp = udev_device_get_devnode(rawDev);
            rfd = open(rp, O_RDWR | O_NONBLOCK);
            if (rfd < 0)
            {
                rfd = open(rp, O_RDONLY | O_NONBLOCK);
                if (rfd < 0)
                {
                    snprintf(errStr, 256, "Unable to open %s@%s: %s\n",
                             device->m_token.getProductName().c_str(), rp, strerror(errno));
                    device->m_devImp.deviceError(errStr);
                    lk.unlock();
                    device->m_initCond.notify_one();
                    udev_device_unref(udevDev);
                    return;
                }
            }
            device->m_rawFd = rfd;
        }

        /* Get joystick file */
        int jfd = 0;
        if (jsDev)
        {
            const char* jp = udev_device_get_devnode(jsDev);
            jfd = open(jp, O_RDWR | O_NONBLOCK);
            if (jfd < 0)
            {
                jfd = open(jp, O_RDONLY | O_NONBLOCK);
                if (jfd < 0)
                {
                    snprintf(errStr, 256, "Unable to open %s@%s: %s\n",
                             device->m_token.getProductName().c_str(), jp, strerror(errno));
                    device->m_devImp.deviceError(errStr);
                    lk.unlock();
                    device->m_initCond.notify_one();
                    udev_device_unref(udevDev);
                    close(rfd);
                    return;
                }
            }
            device->m_joyFd = jfd;
        }

        int maxFd = std::max(rfd, jfd);

        uint8_t numAxis = 0;
        uint8_t axisMap[ABS_CNT];
        uint8_t numButtons = 0;
        uint16_t buttonMap[KEY_MAX - BTN_MISC + 1];

        if (jfd)
        {
            if (ioctl(jfd, JSIOCGAXES, &numAxis) < 0 ||
                ioctl(jfd, JSIOCGAXMAP, axisMap) < 0)
                numAxis = 0;

            if (ioctl(jfd, JSIOCGBUTTONS, &numButtons) < 0 ||
                ioctl(jfd, JSIOCGBTNMAP, buttonMap) < 0)
                numButtons = 0;
        }

        /* Return control to main thread */
        device->m_runningTransferLoop = true;
        lk.unlock();
        device->m_initCond.notify_one();

        /* Report input size */
        size_t readSz = device->m_devImp.getInputBufferSize();
        std::unique_ptr<uint8_t[]> readBuf(new uint8_t[readSz]);

        /* Start transfer loop */
        device->m_devImp.initialCycle();
        while (device->m_runningTransferLoop)
        {
            fd_set readset;
            FD_ZERO(&readset);
            FD_SET(rfd, &readset);
            FD_SET(jfd, &readset);
            struct timeval timeout = {0, 10000};
            if (select(maxFd + 1, &readset, nullptr, nullptr, &timeout) > 0)
            {
                while (rfd)
                {
                    ssize_t sz = read(rfd, readBuf.get(), readSz);
                    if (sz < 0)
                        break;
                    device->m_devImp.receivedHIDReport(readBuf.get(), sz,
                                                       HIDReportType::Input, readBuf[0]);
                }
                while (jfd)
                {
                    js_event ev;
                    ssize_t sz = read(jfd, &ev, sizeof(ev));
                    if (sz < ssize_t(sizeof(ev)))
                        break;
                    if (ev.type == JS_EVENT_BUTTON)
                    {
                        uint16_t udevNum = buttonMap[ev.number];
                        device->m_devImp.receivedHIDValueChange(HIDValueType::Button, udevNum,
                                                                (udevNum < BTN_0 || udevNum > BTN_TRIGGER_HAPPY40) ?
                                                                nullptr : UDevButtonNames[udevNum - BTN_0], ev.value);
                    }
                    else if (ev.type == JS_EVENT_AXIS)
                    {
                        uint16_t udevNum = axisMap[ev.number];
                        device->m_devImp.receivedHIDValueChange(HIDValueType::Axis, udevNum,
                                                                udevNum > ABS_TOOL_WIDTH ?
                                                                nullptr : UDevAxisNames[udevNum], ev.value);
                    }
                }
            }
            device->m_devImp.transferCycle();
        }
        device->m_devImp.finalCycle();

        /* Cleanup */
        close(rfd);
        close(jfd);
        device->m_rawFd = 0;
        device->m_joyFd = 0;
        udev_device_unref(udevDev);
    }

    void _deviceDisconnected()
    {
        m_runningTransferLoop = false;
    }

    bool _sendHIDReport(const uint8_t* data, size_t length, HIDReportType tp, uint32_t)
    {
        if (m_rawFd)
        {
            if (tp == HIDReportType::Feature)
            {
                int ret = ioctl(m_rawFd, HIDIOCSFEATURE(length), data);
                if (ret < 0)
                    return false;
                return true;
            }
            else if (tp == HIDReportType::Output)
            {
                ssize_t ret = write(m_rawFd, data, length);
                if (ret < 0)
                    return false;
                return true;
            }
        }
        return false;
    }

    size_t _receiveHIDReport(uint8_t *data, size_t length, HIDReportType tp, uint32_t message)
    {
        if (m_rawFd)
        {
            if (tp == HIDReportType::Feature)
            {
                data[0] = message;
                int ret = ioctl(m_rawFd, HIDIOCGFEATURE(length), data);
                if (ret < 0)
                    return 0;
                return length;
            }
        }
        return 0;
    }

public:

    HIDDeviceUdev(DeviceToken& token, DeviceBase& devImp)
    : m_token(token),
      m_devImp(devImp),
      m_devPath(token.getDevicePath())
    {
    }

    void _startThread()
    {
        std::unique_lock<std::mutex> lk(m_initMutex);
        DeviceType dType = m_token.getDeviceType();
        if (dType == DeviceType::USB)
            m_thread = std::thread(_threadProcUSBLL, this);
        else if (dType == DeviceType::Bluetooth)
            m_thread = std::thread(_threadProcBTLL, this);
        else if (dType == DeviceType::HID)
            m_thread = std::thread(_threadProcHID, this);
        else
        {
            fprintf(stderr, "invalid token supplied to device constructor");
            abort();
        }
        m_initCond.wait(lk);
    }

    ~HIDDeviceUdev()
    {
        m_runningTransferLoop = false;
        if (m_thread.joinable())
            m_thread.join();
    }


};

std::unique_ptr<IHIDDevice> IHIDDeviceNew(DeviceToken& token, DeviceBase& devImp)
{
    return std::make_unique<HIDDeviceUdev>(token, devImp);
}

}
