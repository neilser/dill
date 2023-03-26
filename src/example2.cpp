#include "dill.h"
#include "spdlog/fmt/fmt.h"

#include <iostream>
#include <sstream>
#include <unordered_map>


std::unordered_map<GUID, DeviceSummary> g_device_info;


std::string type_to_str(JoystickInputType type)
{
    if(type == JoystickInputType::Axis)
    {
        return "Axis";
    }
    else if(type == JoystickInputType::Button)
    {
        return "Button";
    }
    else if(type == JoystickInputType::Hat)
    {
        return "Hat";
    }
    else
    {
        return "Unknown";
    }
}

void device_change_callback(DeviceSummary info, DeviceActionType action)
{
    if(action == DeviceActionType::Disconnected)
    {
        std::cout << fmt::format(
            "{:12s}: {:30s} {}",
            "Disconnected",
            info.name,
            guid_to_string(info.device_guid)
        ) << std::endl;

        return;
    }

    g_device_info[info.device_guid] = info;

    std::cout << fmt::format(
        "{:12s}: {:30s} {}",
        "Connected",
        info.name,
        guid_to_string(info.device_guid)
    ) << std::endl;

    for(size_t i=0; i<info.axis_count; ++i)
    {
        std::cout << ">> " << i << " "
                  << info.axis_map[i].linear_index << " "
                  << info.axis_map[i].axis_index << std::endl;
    }
}

int main(int argc, char *argv[])
{
    //set_input_event_callback(event_callback);
    set_device_change_callback(device_change_callback);
    init();

    for(size_t i=0; i<get_device_count(); ++i)
    {
        auto info = get_device_information_by_index(i);
        std::cout << info.name << std::endl;
    }

    while(true)
    {
        // Print current state of each device
        
        for(auto const& entry : g_device_info)
        {
            auto info = entry.second;
            std::cout << fmt::format(
                "{:30s} {}",
                info.name,
                guid_to_string(info.device_guid)
            )  << std::endl;
            std::stringstream s1(">");
            std::stringstream s2(">");
            for(size_t i=0; i<info.axis_count; ++i)
            {
                s1 << fmt::format("    A {:d}", info.axis_map[i].axis_index);
                s2 << fmt::format(
                    " {: 6d}",
                    get_axis(info.device_guid, info.axis_map[i].axis_index)
                );
            }
            std::cout << s1.str() << std::endl;
            std::cout << s2.str() << std::endl;
        }
        

        /*
        for(size_t i=0; i<get_device_count(); ++i)
        {
            auto info = get_device_information_by_index(i);
            std::cout << fmt::format(
                "{:30s} {} a={:d} b={:d} h={:d}",
                info.name,
                guid_to_string(info.device_guid),
                info.axis_count,
                info.button_count,
                info.hat_count
            ) << std::endl;
        }
        std::cout << "-------------" << std::endl;
        */
        
        SleepEx(1000, 0);
    }

    return 0;
}