idf_component_register(SRCS "GUI_Paint.c" "EPD_config.c" "EPD_2in9.c" "Fonts/font8.c" "Fonts/font12.c"
         "Fonts/font16.c" "Fonts/font20.c" "Fonts/font24.c" "wifiqrcode.c"
                    INCLUDE_DIRS "include" "Fonts"
                    REQUIRES driver)
