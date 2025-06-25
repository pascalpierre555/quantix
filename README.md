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
* main.c: The main application entry point, which initializes NVS, FS and global resources.
* net_task.c: The core networking task.

🔄 Workflow
1. First Boot: The device detects no Wi-Fi configuration and enters AP mode. The UI displays a prompt and a QR code. The user scans the code to connect to the device's AP and configures the home Wi-Fi SSID and password in a web portal.
2. Connection Success: The device connects to the Wi-Fi and registers with the backend server, obtaining a JWT for subsequent API authentication.
3. User Binding: The device retrieves a unique QR code from the server and displays it. The user scans this code with a mobile app (or other means) to bind the device to their user account.
4. Data Sync: The device begins synchronizing the current day's calendar events. If it encounters characters in event titles for which the font is not locally available, font_task automatically requests the font from the font server.
5. Display and Prefetch: The current day's events are displayed on the e-paper. Simultaneously, the device prefetches calendar data for the upcoming and previous few days in the background. All data (calendar and fonts) is saved to LittleFS.
6. Deep Sleep: After all background tasks are complete, the deep_sleep_manager_task in calendar.c checks if the system is idle, sets the GPIO wakeup sources, and puts the device into deep sleep.
7. Wake-up and Interaction: When the user rotates or presses the EC11 encoder, a GPIO interrupt wakes up the ESP32.
8. Offline Operation: Upon waking, the program immediately reads cached data from LittleFS to display the previous or next day's calendar. This provides a responsive experience without waiting for a network connection.
9. Resynchronization: While displaying the new date, a new round of data prefetching is triggered. The device then prepares to enter sleep again, completing a full low-power work cycle.
