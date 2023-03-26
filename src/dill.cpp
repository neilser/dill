
// TODO: xxx I think I need to call CoInitializeEx() in every thread...

#include "dill.h"

#include <atomic>
#include <iostream>
#include <memory>
#include <string.h>
#include <sstream>

#include "spdlog/spdlog.h"
#include "spdlog/sinks/stdout_color_sinks.h" 
#include "spdlog/sinks/basic_file_sink.h"

#if (_WIN32_WINNT >= 0x0602 /*_WIN32_WINNT_WIN8*/)
#include <XInput.h>
#pragma comment(lib,"xinput.lib")
#else
#include <XInput.h>
#pragma comment(lib,"xinput9_1_0.lib")
#endif

#define HID_CLASSGUID {0x4d1e55b2, 0xf16f, 0x11cf,{ 0x88, 0xcb, 0x00, 0x11, 0x11, 0x00, 0x00, 0x30}}
#define CLS_NAME TEXT("GremlinInputListener")
#define HWND_MESSAGE ((HWND)-3)

struct XINPUT_CONTROLLER_STATE
{
    XINPUT_STATE state;
    bool bConnected;
    WORD VID;
    WORD PID;
};

struct XINPUT_VIDPID
{
    WORD VID;
    WORD PID;
};


XINPUT_CONTROLLER_STATE g_XInputControllers[XUSER_MAX_COUNT];
XINPUT_VIDPID g_XInputVIDPID[XUSER_MAX_COUNT]; // Current code can't link numbered XInput devices to VID/PID, so store VID/PID separately
bool g_bXInputPresent;
BYTE g_numXInputFound;

namespace spd = spdlog;

// Setup logger
//auto logger = spd::stdout_color_mt("console");
auto logger = spd::basic_logger_mt("debug", "debug.txt");

// DirectInput system handle
LPDIRECTINPUT8 g_direct_input = nullptr;

// Size of the device object read buffer
static const int g_buffer_size = 64;

// Storage for device data
static DeviceDataStore g_data_store;

// Callback handles
JoystickInputEventCallback g_event_callback = nullptr;
DeviceChangeCallback g_device_change_callback = nullptr;

// Thread handles
HANDLE g_message_thread = NULL;
HANDLE g_joystick_thread = NULL;

// Flag indicating whether or not device initialization is complete
std::atomic<bool> g_initialization_done = false;


DeviceState::DeviceState()
    :   axis(9, 0)
      , button(128, false)
      , hat(4, -1)
{}


std::string error_to_string(DWORD error_code)
{
    static const std::unordered_map<DWORD, std::string> lut{
        // Success codes
        {DI_OK, "The operation completed successfully (DI_OK)"},
        {S_FALSE, "S_FALSE"},
        {DI_BUFFEROVERFLOW, "The device buffer overflowed.  Some input was lost. (DI_BUFFEROVERFLOW)"},
        {DI_PROPNOEFFECT, "The change in device properties had no effect. (DI_PROPNOEFFECT)"},
        // Error codes
        {E_HANDLE, "Invalid handle (E_HANDLE)"},
        {DIERR_INVALIDPARAM, "An invalid parameter was passed to the returning function, or the object was not in a state that admitted the function to be called (DIERR_INVALIDPARAM)"},
        {DIERR_NOTINITIALIZED, "This object has not been initialized (DIERR_NOTINITIALIZED)"},
        {DIERR_OTHERAPPHASPRIO, "Another app has a higher priority level, preventing this call from succeeding. (DIERR_OTHERAPPHASPRIO)"},
        {DIERR_ACQUIRED, "The operation cannot be performed while the device is acquired. (DIERR_ACQUIRED)"},
        {DIERR_DEVICENOTREG, "The device or device instance or effect is not registered with DirectInput. (DIERR_DEVICENOTREG)"},
        {DIERR_INPUTLOST, "Access to the device has been lost. It must be re-acquired. (DIERR_INPUTLOST)"},
        {DIERR_NOTACQUIRED, "The operation cannot be performed unless the device is acquired. (DIERR_NOTACQUIRED)"},
        {DIERR_NOTBUFFERED, "Attempted to read buffered device data from a device that is not buffered. (DIERR_NOTBUFFERED)"},
        {DIERR_NOINTERFACE, "The specified interface is not supported by the object (DIERR_NOINTERFACE)"},
        {DIERR_OBJECTNOTFOUND, "The requested object does not exist. (DIERR_OBJECTNOTFOUND)"},
        {DIERR_UNSUPPORTED, "The function called is not supported at this time (DIERR_UNSUPPORTED)"},
        {DI_POLLEDDEVICE, "The device is a polled device.  As a result, device buffering will not collect any data and event notifications will not be signalled until GetDeviceState is called. (DI_POLLEDDEVICE)"}
    };

    auto itr = lut.find(error_code);
    if(itr != lut.end())
    {
        return itr->second;
    }
    else
    {
        return "Unknown error code";
    }
}

std::string guid_to_string(GUID guid)
{
    LPOLESTR guid_string;
    StringFromCLSID(guid, &guid_string);

    std::wstring unicode_string(guid_string);
    CoTaskMemFree(guid_string);

    return std::string(unicode_string.begin(), unicode_string.end());
}

BOOL on_create_window(HWND window_hdl, LPARAM l_param)
{
    LPCREATESTRUCT params = reinterpret_cast<LPCREATESTRUCT>(l_param);
    GUID interface_class_guid = *(reinterpret_cast<GUID*>(params->lpCreateParams));
    DEV_BROADCAST_DEVICEINTERFACE notification_filter;
    ZeroMemory(&notification_filter, sizeof(notification_filter));
    notification_filter.dbcc_size = sizeof(DEV_BROADCAST_DEVICEINTERFACE);
    notification_filter.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
    notification_filter.dbcc_classguid = interface_class_guid;
    HDEVNOTIFY dev_notify = RegisterDeviceNotification(
        window_hdl,
        &notification_filter,
        DEVICE_NOTIFY_WINDOW_HANDLE
    );

    if(dev_notify == NULL)
    {
        logger->critical("Could not register for devicenotifications!");
        throw std::runtime_error("Could not register for device notifications!");
    }

    return TRUE;
}

BOOL on_device_change(LPARAM l_param, WPARAM w_param)
{
    PDEV_BROADCAST_HDR lpdb = reinterpret_cast<PDEV_BROADCAST_HDR>(l_param);
    PDEV_BROADCAST_DEVICEINTERFACE lpdbv = reinterpret_cast<PDEV_BROADCAST_DEVICEINTERFACE>(lpdb);
    std::string path;
    if(lpdb->dbch_devicetype == DBT_DEVTYP_DEVICEINTERFACE)
    {
        path = std::string(lpdbv->dbcc_name);
        if(w_param == DBT_DEVICEARRIVAL || w_param == DBT_DEVICEREMOVECOMPLETE)
        {
            enumerate_devices();
        }
    }

    return TRUE;
}

LRESULT window_proc(HWND window_hdl, UINT msg_type, WPARAM w_param, LPARAM l_param)
{
    // Window has not yet been created
    if(msg_type == WM_NCCREATE)
    {
        return true;
    }
    // Create the window
    else if(msg_type == WM_CREATE)
    {
        on_create_window(window_hdl, l_param);
    }
    // Device change event
    else if(msg_type == WM_DEVICECHANGE)
    {
        on_device_change(l_param, w_param);
    }

    return 0;
}

void emit_joystick_input_event(DIDEVICEOBJECTDATA const& data, GUID const& guid)
{
    JoystickInputData evt;
    evt.device_guid = guid;

    static std::unordered_map<DWORD, int> axis_id_lookup =
    {
        {FIELD_OFFSET(DIJOYSTATE2, lX), 1},
        {FIELD_OFFSET(DIJOYSTATE2, lY), 2},
        {FIELD_OFFSET(DIJOYSTATE2, lZ), 3},
        {FIELD_OFFSET(DIJOYSTATE2, lRx), 4},
        {FIELD_OFFSET(DIJOYSTATE2, lRy), 5},
        {FIELD_OFFSET(DIJOYSTATE2, lRz), 6},
        {FIELD_OFFSET(DIJOYSTATE2, rglSlider[0]), 7},
        {FIELD_OFFSET(DIJOYSTATE2, rglSlider[1]), 8}
    };
    static std::unordered_map<DWORD, int> hat_id_lookup = 
    {
        {FIELD_OFFSET(DIJOYSTATE2, rgdwPOV[0]), 1},
        {FIELD_OFFSET(DIJOYSTATE2, rgdwPOV[1]), 2},
        {FIELD_OFFSET(DIJOYSTATE2, rgdwPOV[2]), 3},
        {FIELD_OFFSET(DIJOYSTATE2, rgdwPOV[3]), 4}
    };

    // Figure out the type
    if(data.dwOfs < FIELD_OFFSET(DIJOYSTATE2, rgdwPOV))
    {
        evt.input_type = JoystickInputType::Axis;
        evt.input_index = axis_id_lookup[data.dwOfs];
        evt.value = data.dwData;
        g_data_store.state[guid].axis[evt.input_index] = evt.value;
    }
    else if(data.dwOfs < FIELD_OFFSET(DIJOYSTATE2, rgbButtons))
    {
        evt.input_type = JoystickInputType::Hat;
        evt.input_index = hat_id_lookup[data.dwOfs];
        evt.value = data.dwData;
        g_data_store.state[guid].hat[evt.input_index] = evt.value;
    }
    else if(data.dwOfs < FIELD_OFFSET(DIJOYSTATE2, lVX))
    {
        evt.input_type = JoystickInputType::Button;
        evt.input_index = static_cast<UINT8>(data.dwOfs - FIELD_OFFSET(DIJOYSTATE2, rgbButtons) + 1);
        evt.value = (data.dwData & 0x0080) == 0 ? 0 : 1;
        g_data_store.state[guid].button[evt.input_index] = evt.value == 0 ? false : true;
    }
    else
    {
        logger->warn(
            "{}: Unexpected type of input event occurred",
            guid_to_string(guid)
        );
    }

    if(g_event_callback != nullptr)
    {
        g_event_callback(evt);
    }
}

void process_buffered_events(LPDIRECTINPUTDEVICE8 instance, GUID const& guid)
{
    // Poll device to get things going
    auto result = instance->Poll();
    if(FAILED(result))
    {
        logger->error(
            "{} Polling failed, {}",
            guid_to_string(guid),
            error_to_string(result)
        );

        instance->Acquire();
        instance->Poll();
    }

    // Retrieve buffered data
    DIDEVICEOBJECTDATA device_data[g_buffer_size]; 
    DWORD object_count = g_buffer_size;

    while(object_count == g_buffer_size)
    {    
        auto result = instance->GetDeviceData( 
            sizeof(DIDEVICEOBJECTDATA), 
            device_data, 
            &object_count, 
            0
        );
        if(SUCCEEDED(result))
        {
            if(object_count > 0)
            {
                for(size_t i=0; i<object_count; ++i)
                {
                    emit_joystick_input_event(device_data[i], guid);
                }
            }
            if(result == DI_BUFFEROVERFLOW)
            { 
                logger->error(
                    "{}: {}",
                    guid_to_string(guid),
                    error_to_string(result)
                );
            }
        }
        else
        {
            logger->error(
                "{}: {}",
                guid_to_string(guid),
                error_to_string(result)
            );
            object_count = 0;

            // If this failure arose due to buffered reading not being possible
            // revert the device to polled mode
            if(result == DIERR_NOTBUFFERED)
            {
                logger->error(
                    "{} Failed reading device in buffered mode, falling back to polling, {}",
                    guid_to_string(guid),
                    error_to_string(result)
                );
                g_data_store.is_buffered[guid] = false;
            }
        }
    }
}

void poll_device(LPDIRECTINPUTDEVICE8 instance, GUID const& guid)
{
    // Poll device to update internal state
    auto result = instance->Poll();
    if(FAILED(result))
    {
        logger->error(
            "{} Polling failed, {}",
            guid_to_string(guid),
            error_to_string(result)
        );

        instance->Acquire();
        instance->Poll();
    }

    // Obtain device state
    DIJOYSTATE2 state;
    result = instance->GetDeviceState(sizeof(DIJOYSTATE2), &state);
    if(FAILED(result))
    {
        logger->error(
            "{} Retrieving device state failed, {}",
            guid_to_string(guid),
            error_to_string(result)
        );
        return;
    }

    // Event structure to use with the callback
    JoystickInputData evt;
    evt.device_guid = guid;

    // Detect and handle axis state changes
    for(size_t i=0; i<g_data_store.cache[guid].axis_count; ++i)
    {
        auto axis_index = g_data_store.cache[guid].axis_map[i].axis_index;
        auto value = 0;
        if     (axis_index == 1) { value = state.lX; }
        else if(axis_index == 2) { value = state.lY; }
        else if(axis_index == 3) { value = state.lZ; }
        else if(axis_index == 4) { value = state.lRx; }
        else if(axis_index == 5) { value = state.lRy; }
        else if(axis_index == 6) { value = state.lRz; }
        else if(axis_index == 7) { value = state.rglSlider[0]; }
        else if(axis_index == 8) { value = state.rglSlider[1]; }

        if(g_data_store.state[guid].axis[axis_index] != value)
        {
            evt.input_type = JoystickInputType::Axis;
            evt.input_index = static_cast<UINT8>(axis_index);
            evt.value = value;
            g_data_store.state[guid].axis[axis_index] = value;

            if(g_event_callback != nullptr)
            {
                g_event_callback(evt);
            }
        }
    }

    // Detect and handle button state changes
    for(size_t i=0; i<g_data_store.cache[guid].button_count; ++i)
    {
        auto is_pressed = (state.rgbButtons[i] & 0x0080) == 0 ? false : true;
        if(g_data_store.state[guid].button[i+1] != is_pressed)
        {
            evt.input_type = JoystickInputType::Button;
            evt.input_index = static_cast<UINT8>(i + 1);
            evt.value = is_pressed;
            g_data_store.state[guid].button[evt.input_index] = is_pressed;

            if(g_event_callback != nullptr)
            {
                g_event_callback(evt);
            }
        }
    }

    // Detect and handle hat state changes
    for(size_t i=0; i<g_data_store.cache[guid].hat_count; ++i)
    {
        LONG direction = state.rgdwPOV[i];
        if(direction < 0 || direction > 36000)
        {
            direction = -1;
        }
        if(g_data_store.state[guid].hat[i+1] != direction)
        {
            evt.input_type = JoystickInputType::Hat;
            evt.input_index = static_cast<UINT8>(i+1);
            evt.value = direction;
            g_data_store.state[guid].hat[evt.input_index] = evt.value;

            if(g_event_callback != nullptr)
            {
                g_event_callback(evt);
            }
        }
    }
}

DWORD WINAPI joystick_update_thread(LPVOID l_param)
{
    while(true)
    {
        if(g_initialization_done)
        {
            for(auto & entry : g_data_store.device_map)
            {
                if(!g_data_store.is_ready[entry.first])
                {
                    logger->info(
                        "Skipping device {}, not yet fully initialized",
                        guid_to_string(entry.first)
                    );
                    continue;
                }

                if(g_data_store.is_buffered[entry.first])
                {
                    process_buffered_events(entry.second, entry.first);
                }
                else
                {
                    poll_device(entry.second, entry.first);
                }
            }
        }
        SleepEx(4, false);
    }

    return 0;
}

DWORD WINAPI message_handler_thread(LPVOID l_param)
{
    // Initialize the window to receive messages through
    HWND hWnd = create_window();
    if(hWnd == NULL)
    {
        logger->critical("Could not create message window!");
        throw std::runtime_error("Could not create message window!");
    }

    // Start the message loop
    MSG msg;
    while(GetMessage(&msg, NULL, 0, 0))
    {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    return 0;
}

HWND create_window()
{
    WNDCLASSEX wx;
    ZeroMemory(&wx, sizeof(wx));

    wx.cbSize = sizeof(WNDCLASSEX);
    wx.lpfnWndProc = reinterpret_cast<WNDPROC>(window_proc);
    wx.hInstance = reinterpret_cast<HINSTANCE>(GetModuleHandle(0));
    wx.style = CS_HREDRAW | CS_VREDRAW;
    wx.hInstance = GetModuleHandle(0);
    wx.hbrBackground = (HBRUSH)(COLOR_WINDOW);
    wx.lpszClassName = CLS_NAME;

    GUID guid = HID_CLASSGUID;
    HWND window_hdl = NULL;
    if(RegisterClassEx(&wx))
    {
        window_hdl = CreateWindow(
            CLS_NAME,
            TEXT("DevNotifWnd"),
            WS_ICONIC,
            0,
            0,
            CW_USEDEFAULT,
            0,
            HWND_MESSAGE,
            NULL,
            GetModuleHandle(0),
            reinterpret_cast<void*>(&guid)
        );
    }

    return window_hdl;
}

BOOL CALLBACK set_axis_range(LPCDIDEVICEOBJECTINSTANCE lpddoi, LPVOID pvRef)
{
    LPDIRECTINPUTDEVICE8 device = reinterpret_cast<LPDIRECTINPUTDEVICE8>(pvRef);
    if(lpddoi->dwType & DIDFT_AXIS)
    {
        DIPROPRANGE range;

        ZeroMemory(&range, sizeof(range));
        range.diph.dwSize = sizeof(range);
        range.diph.dwHeaderSize = sizeof(range.diph);
        range.diph.dwObj = lpddoi->dwType;
        range.diph.dwHow = DIPH_BYID;
        range.lMin = -32768;
        range.lMax = 32767;

        auto result = device->SetProperty(DIPROP_RANGE, &range.diph);
        if(FAILED(result))
        {
            logger->error(
                "Error while setting axis range, {}",
                error_to_string(result)
            );
            return DIENUM_CONTINUE;
        }
    }
    return DIENUM_CONTINUE;
}

void initialize_device(GUID guid, std::string name)
{
    // Prevent any operations on this device until initialization is done
    g_data_store.is_ready[guid] = false;

    // Check if we have an existing instance in the device map
    auto execute_callback = true;
    {
        if(g_data_store.device_map.find(guid) != g_data_store.device_map.end())
        {
            execute_callback = false;
            auto device = g_data_store.device_map[guid];
            auto result = device->Unacquire();
            if(FAILED(result))
            {
                logger->error(
                    "{}: Failed unacquiring device, {}",
                    guid_to_string(guid),
                    error_to_string(result)
                );
            }
            g_data_store.device_map.erase(guid);
        }
    }

    // Create joystick device
    LPDIRECTINPUTDEVICE8 device = nullptr;
    auto result = g_direct_input->CreateDevice(
        guid,
        &device,
        nullptr
    );
    if(FAILED(result))
    {
        logger->error(
            "{}: Failed creating device, {}",
            guid_to_string(guid),
            error_to_string(result)
        );
    }

    // Store device in the data storage
    g_data_store.device_map[guid] = device;

    // Setting cooperation level
    result = device->SetCooperativeLevel(
        NULL,
        DISCL_NONEXCLUSIVE | DISCL_BACKGROUND
    );
    if(FAILED(result))
    {
        logger->error(
            "{}: Failed setting cooperative level, {}",
            guid_to_string(guid),
            error_to_string(result)
        );
    }

    // Set data format for reports
    result = device->SetDataFormat(&c_dfDIJoystick2);
    if(FAILED(result))
    {
        logger->error(
            "{}: Error while setting data format, {}",
            guid_to_string(guid),
            error_to_string(result)
        );
    }
 
    // Set device buffer size property
    DIPROPDWORD prop_word;
    DIPROPHEADER prop_header;
    prop_header.dwSize = sizeof(DIPROPDWORD);
    prop_header.dwHeaderSize = sizeof(DIPROPHEADER);
    prop_header.dwObj = 0;
    prop_header.dwHow = DIPH_DEVICE;
    prop_word.diph = prop_header;
    prop_word.dwData = g_buffer_size;
    // By default assume the device supports buffered reading and revert
    // to polled upon failure
    g_data_store.is_buffered[guid] = true;
    result = device->SetProperty(DIPROP_BUFFERSIZE, &prop_header);
    if(FAILED(result))
    {
        logger->error(
            "{}: Error while setting device properties, {}",
            guid_to_string(guid),
            error_to_string(result)
        );
    }
    if(result == DI_POLLEDDEVICE)
    {
        logger->warn("Device {} is not buffered", guid_to_string(guid));
        g_data_store.is_buffered[guid] = false;
    }

    // Acquire device
    result = device->Acquire();
    if(FAILED(result))
    {
        logger->error(
            "{}: Failed to acquire the device, {}",
            guid_to_string(guid),
            error_to_string(result)
        );
    }

    // Query device capabilities
    DIDEVCAPS capabilities;
    capabilities.dwSize = sizeof(DIDEVCAPS);
    result = device->GetCapabilities(&capabilities);
    if(FAILED(result))
    {
        logger->error(
            "{}: Failed to obtain device capabilities, {}",
            guid_to_string(guid),
            error_to_string(result)
        );
    }

    // Create device summary report
    DeviceSummary info;
    info.device_guid = guid;
    info.vendor_id = get_vendor_id(device, guid);
    info.product_id = get_product_id(device, guid);
    info.joystick_id = get_joystick_id(device, guid);
    strcpy_s(info.name, MAX_PATH, name.c_str());
    info.axis_count = 0;
    for(int i=0; i<8; ++i)
    {
        info.axis_map[i].linear_index = 0;
        info.axis_map[i].axis_index = 0;
    }

    auto axis_indices = used_axis_indices(guid);

    // Do some error checking on axis counts
    if(axis_indices.size() > 8)
    {
        logger->error(
            "{} {}: Invalid number of axis reported, {} > 8",
            info.name,
            guid_to_string(info.device_guid),
            axis_indices.size()
        );
        axis_indices.resize(8);
    }
    if(capabilities.dwAxes > 8)
    {
        logger->error(
            "{} {}: Reports more then 8 axis, {}",
            info.name,
            guid_to_string(info.device_guid),
            capabilities.dwAxes
        );
    }

    // Handle all the various ways in which device can misreport device axes information
    // 1. dwAxes reports more then 8 axes simply discard dwAxes data an only use
    //    axis_indices
    // 2. dwAxes and axis_indices value disagree and dwAxes is > 0 and < 9 while
    //    axis_info is empty, hope for the best and assume we have dwAxes linear
    //    axes present and fix axis_information

    // There is something wrong with the reported axis counts, many ways to
    // fix the discrepancy.
    if(axis_indices.size() != capabilities.dwAxes)
    {
        if(capabilities.dwAxes > 0 && capabilities.dwAxes < 9 && axis_indices.size() == 0)
        {
            // No axis map data enumerated but valid looking axis count reported, lets
            // hope these are all one after the other
            info.axis_count = capabilities.dwAxes;
            for(size_t i=0; i<info.axis_count; ++i)
            {
                info.axis_map[i].linear_index = i+1;
                info.axis_map[i].axis_index = i+1;
            }

            logger->warn(
                "{} {}: Axis information, invalid hoping for the best, capabilities={} enumerated={}",
                info.name,
                guid_to_string(info.device_guid),
                capabilities.dwAxes,
                axis_indices.size()
            );
        }

        else
        {
            // Some other invalid axis count information returned, simply trust the
            // axis enumeration
            info.axis_count = axis_indices.size();
            for(size_t i=0; i<axis_indices.size(); ++i)
            {
                info.axis_map[i].linear_index = i+1;
                info.axis_map[i].axis_index = axis_indices[i];
            }
            logger->warn(
                "{} {}: Overriding reported number of axes,  capabilities={} enumerated={}",
                info.name,
                guid_to_string(info.device_guid),
                capabilities.dwAxes,
                axis_indices.size()
            );
        }

    }
    // Both axis counts agree, so we'll just use those
    else
    {
        info.axis_count = capabilities.dwAxes;
        for(size_t i=0; i<axis_indices.size(); ++i)
        {
            info.axis_map[i].linear_index = i+1;
            info.axis_map[i].axis_index = axis_indices[i];
        }
    }


    info.button_count = capabilities.dwButtons;
    info.hat_count = capabilities.dwPOVs;

    // Write device summary to debug file
    logger->info("Device summary: {} {}", info.name,guid_to_string(guid));
    logger->info("Axis={} Buttons={} Hats={}", info.axis_count, info.button_count, info.hat_count);
    logger->info("Axis map");
    for(auto const& entry : info.axis_map)
    {
        logger->info("  linear={} id={}", entry.linear_index, entry.axis_index);
    }


    // Add device to list of active guids
    bool add_guid = true;
    for(size_t i=0; i<g_data_store.active_guids.size(); ++i)
    {
        if(g_data_store.active_guids[i] == guid)
        {
            add_guid = false;
            break;
        }
    }
    if(add_guid)
    {
        g_data_store.active_guids.push_back(guid);
    }

    // Set the axis range for each axis of the device
    device->EnumObjects(set_axis_range, device, DIDFT_ALL);

    g_data_store.cache[guid] = info;
    if(g_device_change_callback != nullptr && execute_callback)
    {
        g_device_change_callback(info, DeviceActionType::Connected);
    }

    // Allow operating on the device
    g_data_store.is_ready[guid] = true;
}

BOOL CALLBACK handle_device_cb(LPCDIDEVICEINSTANCE instance, LPVOID data)
{
    // Convert user data pointer to data storage device
    std::unordered_map<GUID, bool>* current_devices = 
        reinterpret_cast<std::unordered_map<GUID, bool>*>(data);

    logger->info(
        "{}: Processing device: {}",
        guid_to_string(instance->guidInstance),
        std::string(instance->tszProductName)
    );

    // Aggregate device information
    (*current_devices)[instance->guidInstance] = true;
    initialize_device(
        instance->guidInstance,
        std::string(instance->tszInstanceName)
    );

    // Continue to enumerate devices
    return DIENUM_CONTINUE;
}

void find_xinput_devices()
{
    // Check all input devices and record the VID and PID for any which have the signature indicating that they are XInput
    UINT numRawDevices = 0;
    // Info for connected XInput devices will be rebuilt each time we call this function
    g_numXInputFound = 0;
    for (UINT i = 0; i < XUSER_MAX_COUNT; i++)
    {
        g_XInputVIDPID[i].VID = 0;
        g_XInputVIDPID[i].PID = 0;
    }
    // First, get just the number of raw input devices:
    volatile UINT result = GetRawInputDeviceList(nullptr, &numRawDevices, sizeof(RAWINPUTDEVICELIST));
    if (numRawDevices > 0) {
        numRawDevices++; // in case another device has just been plugged in
        std::unique_ptr<RAWINPUTDEVICELIST[]> device_list(new RAWINPUTDEVICELIST[numRawDevices]);
        result = GetRawInputDeviceList(device_list.get(), &numRawDevices, sizeof(RAWINPUTDEVICELIST));
        numRawDevices = result;
        for (UINT i = 0; i < numRawDevices; i++) {
            if (device_list[i].dwType == RIM_TYPEHID) {
                // Want to fetch the device name and check it for "IG_"
                UINT namelen = 0;
                // Get the name length:
                result = GetRawInputDeviceInfo(device_list[i].hDevice, RIDI_DEVICENAME, nullptr, &namelen);
                // And get the name itself:
                std::unique_ptr<char[]> device_name(new char[namelen]);
                result = GetRawInputDeviceInfo(device_list[i].hDevice, RIDI_DEVICENAME, device_name.get(), &namelen);
                // Check for IG_ in name
                if (strstr(device_name.get(), "IG_"))
                {
                    // Found IG_, so fetch and store the VID and PID...
                    OutputDebugString("Found it!\n");
                    // Get the device info:
                    RID_DEVICE_INFO device_info;
                    UINT devinfolen = sizeof(RID_DEVICE_INFO);
                    result = GetRawInputDeviceInfo(device_list[i].hDevice, RIDI_DEVICEINFO, &device_info, &devinfolen);
                    if (result > 0) {
                        g_XInputVIDPID[g_numXInputFound].VID = (WORD) device_info.hid.dwVendorId;
                        g_XInputVIDPID[g_numXInputFound].PID = (WORD) device_info.hid.dwProductId;
                        g_numXInputFound++;
                    }
                }
            }
        }
    }
}

void enumerate_devices()
{
    g_initialization_done = false;

    // Register with the DirectInput system, creating an instance to
    // interface with it
    if(g_direct_input == nullptr)
    {
        auto result = DirectInput8Create(
            GetModuleHandle(nullptr),
            DIRECTINPUT_VERSION,
            IID_IDirectInput8,
            (VOID**)&g_direct_input,
            nullptr
        );
        if(FAILED(result))
        {
            logger->critical(
                "Failed registering with DirectInput, {}",
                error_to_string(result)
            );
            throw std::runtime_error("Failed registering with DirectInput");
        }
    }

    // Update the list of XInput devices so we can ignore them:
    // TODO: I think I need to check presence of XInput every time we enumerate devices, not just if init() found any
    if (g_bXInputPresent)
        find_xinput_devices();

    std::unordered_map<GUID, bool> current_devices;
    auto result = g_direct_input->EnumDevices(
        DI8DEVCLASS_GAMECTRL,
        handle_device_cb,
        &current_devices,
        DIEDFL_ATTACHEDONLY
    );
    if(FAILED(result))
    {
        logger->error("Failure occured while discovering devices, {}", error_to_string(result));
    }

    // Get rid of devices we no longer have from the global map
    std::vector<GUID> guid_to_remove;
    for(auto const& entry : g_data_store.device_map)
    {
        if(current_devices.find(entry.first) == current_devices.end())
        {
            guid_to_remove.push_back(entry.first);
        }
    }
    for(auto const& guid : guid_to_remove)
    {
        logger->info("{}: Removing device", guid_to_string(guid));
        g_data_store.device_map.erase(guid);

        // Emit DeviceInformation, copy existing device data if we know about
        // the device and have an existing record, otherwise return a shell
        DeviceSummary di;
        if(g_data_store.cache.find(guid) != g_data_store.cache.end())
        {
            di = g_data_store.cache[guid];
        }
        else
        {
            di.device_guid = guid;
            strcpy_s(di.name, MAX_PATH, "Unknown");
        }

        // Remove guid from list of active ones
        for(size_t i=0; i<g_data_store.active_guids.size(); ++i)
        {
            if(g_data_store.active_guids[i] == guid)
            {
                g_data_store.active_guids.erase(
                    g_data_store.active_guids.begin() + i
                );
                break;
            }
        }
 
        if(g_device_change_callback != nullptr)
        {
            g_device_change_callback(di, DeviceActionType::Disconnected);
        }
    }

    g_initialization_done = true;
}

BOOL init()
{
    g_initialization_done = false;
    logger->info("Initializing DILL v1.3");

    // Initialize COM
    // TODO: may have to consider getting the client code to call CoUninitialize() since no place here in dill to do it
    // (unless one is added of course) so perhaps CoInitializeEx() belongs in the client code too?
    // TODO: could be rrealllly tidy and decode the errors from CoInitializeEx()
    HRESULT hr;
    if (FAILED(hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED)))
        logger->error("CoInitializeEx error {}", hr);
    
    // Check if any XInput devices are connected
    // TODO: shouldn't this check be part of the enumerate() stuff, since the answer can change? (if it's needed at all)
    g_bXInputPresent = false;
    DWORD dwResult;
    for (DWORD i = 0; i < XUSER_MAX_COUNT; i++)
    {
        // Call XInputGetState and check if it succeeds; if yes that controller is connected
        dwResult = XInputGetState(i, &g_XInputControllers[i].state);

        if (dwResult == ERROR_SUCCESS)
        {
            g_XInputControllers[i].bConnected = true;
            g_bXInputPresent = true;
            logger->info("Found XInput device {}", i);
        }
        else
            g_XInputControllers[i].bConnected = false;
    }

    // Force an update of device enumeration to bootstrap everything
    enumerate_devices();

    // Start joystick update loop thread
    g_joystick_thread = CreateThread(
            NULL,
            0,
            joystick_update_thread,
            NULL,
            0,
            NULL
    );
    if(g_joystick_thread == NULL)
    {
        logger->error("Creating joystick thread failed {}", GetLastError());
        return FALSE;
    }

    // Start joystick update loop thread
    g_message_thread = CreateThread(
            NULL,
            0,
            message_handler_thread,
            NULL,
            0,
            NULL
    );
    if(g_message_thread == NULL)
    {
        logger->error(
            "Creating message handler thread failed {}",
            GetLastError()
        );
        return FALSE;
    }

    g_initialization_done = true;
    return TRUE;
}

void set_input_event_callback(JoystickInputEventCallback cb)
{
    logger->info("Setting event callback");
    g_event_callback = cb;
}

void set_device_change_callback(DeviceChangeCallback cb)
{
    logger->info("Setting device change callback");
    g_device_change_callback = cb;
}

DeviceSummary get_device_information_by_index(size_t index)
{
    if(index < 0 || index >= g_data_store.active_guids.size())
    {
        return DeviceSummary();
    }
    auto guid = g_data_store.active_guids[index];
    return get_device_information_by_guid(guid);
}

DeviceSummary get_device_information_by_guid(GUID guid)
{
    if(g_data_store.cache.find(guid) == g_data_store.cache.end())
    {
        return DeviceSummary();
    }

    return g_data_store.cache[guid];
}


size_t get_device_count()
{
    return g_data_store.active_guids.size();
}

bool device_exists(GUID guid)
{
    bool exists = false;
    for(auto const& dev_guid : g_data_store.active_guids)
    {
        if(dev_guid == guid)
        {
            exists = true;
        }
    }

    return exists;
}

LONG get_axis(GUID guid, DWORD index)
{
    if(index < 1 || index > 8)
    {
        logger->error(
            "{}: Requested invalid axis index {}",
            guid_to_string(guid),
            index
        );
        return 0;
    }

    return g_data_store.state[guid].axis[index];
}

bool get_button(GUID guid, DWORD index)
{
    if(index < 0 || index >= 128)
    {
        logger->error(
            "{}: Requested invalid button index {}",
            guid_to_string(guid),
            index
        );
        return false;
    }

    return g_data_store.state[guid].button[index];
}

LONG get_hat(GUID guid, DWORD index)
{
    if(index < 0 || index >= 4)
    {
        logger->error(
            "{}: Requested invalid hat index {}",
            guid_to_string(guid),
            index
        );
        return -1;
    }

    return g_data_store.state[guid].hat[index];
}

std::vector<int> used_axis_indices(GUID guid)
{
    DIJOYSTATE2 state;
    g_data_store.device_map[guid]->Poll();
    auto result = g_data_store.device_map[guid]->GetDeviceState(
        sizeof(state),
        &state
    );

    if(FAILED(result))
    {
        logger->critical("Failed determining used axes indices.");
        return {};
    }

    std::vector<int> used_indices;
    if(state.lX != 0)           { used_indices.push_back(1); }
    if(state.lY != 0)           { used_indices.push_back(2); }
    if(state.lZ != 0)           { used_indices.push_back(3); }
    if(state.lRx != 0)          { used_indices.push_back(4); }
    if(state.lRy != 0)          { used_indices.push_back(5); }
    if(state.lRz != 0)          { used_indices.push_back(6); }
    if(state.rglSlider[0] != 0) { used_indices.push_back(7); }
    if(state.rglSlider[1] != 0) { used_indices.push_back(8); }

    return used_indices;
}

DWORD get_vendor_id(LPDIRECTINPUTDEVICE8 device, GUID guid)
{
    DIPROPDWORD data;

    ZeroMemory(&data, sizeof(DIPROPDWORD));
    data.diph.dwSize = sizeof(DIPROPDWORD);
    data.diph.dwHeaderSize = sizeof(DIPROPHEADER);
    data.diph.dwObj = 0;
    data.diph.dwHow = DIPH_DEVICE;

    auto result = device->GetProperty(
        DIPROP_VIDPID,
        &data.diph
    );

    if(FAILED(result))
    {
        logger->critical(
            "{} Failed retrieving joystick vendor id data, {}",
            guid_to_string(guid),
            error_to_string(result)
        );
        return 0;
    }

    return LOWORD(data.dwData);
}

DWORD get_product_id(LPDIRECTINPUTDEVICE8 device, GUID guid)
{
    DIPROPDWORD data;

    ZeroMemory(&data, sizeof(DIPROPDWORD));
    data.diph.dwSize = sizeof(DIPROPDWORD);
    data.diph.dwHeaderSize = sizeof(DIPROPHEADER);
    data.diph.dwObj = 0;
    data.diph.dwHow = DIPH_DEVICE;

    auto result = device->GetProperty(
        DIPROP_VIDPID,
        &data.diph
    );

    if(FAILED(result))
    {
        logger->critical(
            "{} Failed retrieving joystick product id data, {}",
            guid_to_string(guid),
            error_to_string(result)
        );
        return 0;
    }

    return HIWORD(data.dwData);
}

DWORD get_joystick_id(LPDIRECTINPUTDEVICE8 device, GUID guid)
{
    DIPROPDWORD data;

    ZeroMemory(&data, sizeof(DIPROPDWORD));
    data.diph.dwSize = sizeof(DIPROPDWORD);
    data.diph.dwHeaderSize = sizeof(DIPROPHEADER);
    data.diph.dwObj = 0;
    data.diph.dwHow = DIPH_DEVICE;

    auto result = device->GetProperty(
        DIPROP_JOYSTICKID,
        &data.diph
    );

    if(FAILED(result))
    {
        logger->critical(
            "{} Failed retrieving joystick id data, {}",
            guid_to_string(guid),
            error_to_string(result)
        );
        return 0;
    }

    return data.dwData;
}