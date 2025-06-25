# Quantix
Quantix is an ESP32-based e-paper calendar application that integrates with Google Calendar and supports multilingual character rendering. It is designed to be a stylish and power-efficient desktop display or personal information board.

## Features
- **Dynamic Font System**  
  Automatically detects missing Chinese characters in the display text and downloads the corresponding bitmap data from a server in real-time. The data is then cached locally using LittleFS for future use.

- **Low-Power Design**  
  After updating calendar data and refreshing the display, the system enters deep sleep mode to greatly extend battery life — ideal for always-on, battery-powered use.

- **Effortless Setup**  
  First-time users can easily configure Wi-Fi and link their Google account by simply scanning a QR code with their phone, streamlining the setup process for a smooth and intuitive experience.

## 🛠️ Hardware Requirements
* ESP32 development board
* 2.9-inch SPI E-Paper Display (e.g., Waveshare e-Paper E-Ink Display Module V2)
* EC11 Rotary Encoder (with push-button)

## Software Architecture
This project consists of an ESP32-based client and a Flask-based server.
The directory structure is as follows:

```
├── quantix
│   ├── CMakeLists.txt
│   ├── components                ← ESP32 components (drivers, wifi manager, JWT, e-paper, etc.)
│   ├── main                      ← Main application source code for ESP32
│   │   ├── calendar.c
│   │   ├── font_task.c
│   │   ├── main.c                
│   │   ├── net_task.c            
│   │   └── ui_task.c
│   ├── managed_components        ← Third-party components managed by the build system
│   ├── sdkconfig
├── quantix-server
│   ├── app.py                    ← Flask application entry point    
│   └── unifont_jp-16.0.04.otf    ← Default font for Chinese layout, can be replaced by any monospace font
└── README.md

```
* ```main.c```: The main application entry point, which initializes NVS, FS and global resources. Also handles special logic after waking up from deep sleep.
* ```net_task.c```: The core networking task. Manages all HTTP requests, handles JWT keys for server login, and monitors Wi-Fi and server connection status with automatic reconnection on disconnection.
* ```calendar.c```: The main calendar logic task. Manages calendar display functionality, including the currently shown date and automatic time synchronization.
* ```font_task.c```: The dynamic font manager. Handles font storage and caching, and is responsible for sending HTTP requests via net_task to download missing fonts.
* ```ui_task.c```: The UI display task. Manages e-paper display updates and GUI definitions.

## 🔄 Workflow
1. First Boot: The device detects no Wi-Fi configuration and enters AP mode. The UI displays a prompt and a QR code. The user scans the code to connect to the device's AP and configures the home Wi-Fi SSID and password in a web portal.
2. Connection Success: The device connects to the Wi-Fi and registers with the backend server, obtaining a JWT for subsequent API authentication.
3. User Binding: The device retrieves a unique QR code from the server and displays it. The user scans this code with a mobile app (or other means) to bind the device to their user account.
4. Data Sync: The device begins synchronizing the current day's calendar events. If it encounters characters in event titles for which the font is not locally available, font_task automatically requests the font from the font server.
5. Display and Prefetch: The current day's events are displayed on the e-paper. Simultaneously, the device prefetches calendar data for the upcoming and previous few days in the background. All data (calendar and fonts) is saved to LittleFS.
6. Deep Sleep: After all background tasks are complete, the deep_sleep_manager_task in calendar.c checks if the system is idle, sets the GPIO wakeup sources, and puts the device into deep sleep.
7. Wake-up and Interaction: When the user rotates or presses the EC11 encoder, a GPIO interrupt wakes up the ESP32.
8. Offline Operation: Upon waking, the program immediately reads cached data from LittleFS to display the previous or next day's calendar. This provides a responsive experience without waiting for a network connection.
9. Resynchronization: While displaying the new date, a new round of data prefetching is triggered. The device then prepares to enter sleep again, completing a full low-power work cycle.
   
## Acknowledgements

This project is built upon the following open-source libraries and components. A special thanks to their developers and contributors:

*   [**ESP-IDF**](https://github.com/espressif/esp-idf): The official IoT Development Framework by Espressif, providing the underlying drivers and system services for this project.
*   [**FreeRTOS**](https://www.freertos.org/): Included as part of ESP-IDF, it provides the core multitasking and synchronization mechanisms.
*   [**cJSON**](https://github.com/DaveGamble/cJSON): A lightweight and fast JSON parser for C, used for handling all API data exchange with the backend server.
*   [**esp-littlefs**](https://github.com/joltwallet/esp_littlefs): An ESP-IDF component for the LittleFS file system, enabling reliable local data caching (calendar events, fonts) in this project.
*   [**esp32-wifi-manager**](https://github.com/tonyp7/esp32-wifi-manager): A powerful Wi-Fi connection management component that simplifies the initial device provisioning process.
*   [**Waveshare E-Paper Driver**](https://www.waveshare.com/wiki/2.9inch_e-Paper_Module_Manual): The drivers and graphics libraries related to `EPD_2in9` and `GUI_Paint` are based on the official example code provided by Waveshare.

