/* LVGL Example project. IMPORTANT: This is a complex UX contruct and renders slow on epaper
 * Modify CMakeLists and point it to epaper_demo.cpp if you want a simple demo that renders fast
 *
 * Basic project to test LVGL on ESP32 based projects.
 *
 * This example code is in the Public Domain (or CC0 licensed, at your option.)
 *
 * Unless required by applicable law or agreed to in writing, this
 * software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
 * CONDITIONS OF ANY KIND, either express or implied.
 */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_freertos_hooks.h"
#include "freertos/semphr.h"
#include "esp_system.h"
#include "driver/gpio.h"

// Should match with your epaper module, size
// CalEPD try: Works but it really needs to be implemented in test il3820.c not here: 
//#include <gdew0583t7.h>

/* Littlevgl specific */
#ifdef LV_LVGL_H_INCLUDE_SIMPLE
#include "lvgl.h"
#else
#include "lvgl/lvgl.h"
#endif

#include "lvgl_helpers.h"

//#ifndef CONFIG_LV_TFT_DISPLAY_MONOCHROME
    #if defined CONFIG_LV_USE_DEMO_WIDGETS
        #include "lv_examples/src/lv_demo_widgets/lv_demo_widgets.h"
        // Leave a new define to make only epaper demos
    #elif defined CONFIG_LV_USE_DEMO_KEYPAD_AND_ENCODER
        #include "lv_examples/src/lv_demo_keypad_encoder/lv_demo_keypad_encoder.h"
    #elif defined CONFIG_LV_USE_DEMO_BENCHMARK
        #include "lv_examples/src/lv_demo_benchmark/lv_demo_benchmark.h"
    #elif defined CONFIG_LV_USE_DEMO_STRESS
        #include "lv_examples/src/lv_demo_stress/lv_demo_stress.h"
    #else
        #error "No demo application selected."
    #endif
//#endif

/*********************
 *      DEFINES
 *********************/
#define TAG "demo"
#define LV_TICK_PERIOD_MS 1


extern "C"
{
    void app_main();
}
/**********************
 *  STATIC PROTOTYPES
 **********************/
static void lv_tick_task(void *arg);
static void guiTask(void *pvParameter);
static void create_demo_application(void);

/**********************
 *   APPLICATION MAIN
 **********************/

void app_main() {
     printf("app_main started. DISP_BUF_SIZE:%d LV_HOR_RES_MAX:%d V_RES_MAX:%d\n", DISP_BUF_SIZE, LV_HOR_RES_MAX, LV_VER_RES_MAX);
   
    /* If you want to use a task to create the graphic, you NEED to create a Pinned task
     * Otherwise there can be problem such as memory corruption and so on.
     * NOTE: When not using Wi-Fi nor Bluetooth you can pin the guiTask to core 0 */
    xTaskCreatePinnedToCore(guiTask, "gui", 4096*2, NULL, 0, NULL, 1);
}

/* Creates a semaphore to handle concurrent call to lvgl stuff
 * If you wish to call *any* lvgl function from other threads/tasks
 * you should lock on the very same semaphore! */
SemaphoreHandle_t xGuiSemaphore;

static void guiTask(void *pvParameter) {

    (void) pvParameter;
    xGuiSemaphore = xSemaphoreCreateMutex();

    lv_init();

    /* Initialize SPI or I2C bus used by the drivers */
    lvgl_driver_init();

    lv_color_t* buf1 = (lv_color_t*) heap_caps_malloc(DISP_BUF_SIZE * sizeof(lv_color_t), MALLOC_CAP_DMA);
    assert(buf1 != NULL);

    static lv_color_t *buf2 = NULL;

    static lv_disp_buf_t disp_buf;

    /* Actual size in pixels, not bytes. */
    // Missing 56 PX between chunks, why? + (LV_HOR_RES_MAX*56)
    uint32_t size_in_px = LV_HOR_RES_MAX*(LV_VER_RES_MAX/10);

    /* Initialize the working buffer depending on the selected display.
     * NOTE: buf2 == NULL when using monochrome displays. */
    lv_disp_buf_init(&disp_buf, buf1, buf2, size_in_px);

    lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);
    disp_drv.flush_cb = disp_driver_flush;

    /* When using a monochrome display we need to register the callbacks:
     * - rounder_cb
     * - set_px_cb */
#ifdef CONFIG_LV_TFT_DISPLAY_MONOCHROME
    disp_drv.rounder_cb = disp_driver_rounder;
    disp_drv.set_px_cb = disp_driver_set_px;
#elif CONFIG_LV_EPAPER_EPDIY_DISPLAY_CONTROLLER || CONFIG_LV_EPAPER_CALEPD_DISPLAY_CONTROLLER
    disp_drv.set_px_cb = disp_driver_set_px;
#endif
    
    disp_drv.buffer = &disp_buf;
    lv_disp_drv_register(&disp_drv);

    /* Register an input device when enabled on the menuconfig */
#if CONFIG_LV_TOUCH_CONTROLLER != TOUCH_CONTROLLER_NONE
    lv_indev_drv_t indev_drv;
    lv_indev_drv_init(&indev_drv);
    indev_drv.read_cb = touch_driver_read;
    indev_drv.type = LV_INDEV_TYPE_POINTER;
    lv_indev_drv_register(&indev_drv);
#endif

    /* Create and start a periodic timer interrupt to call lv_tick_inc */
    const esp_timer_create_args_t periodic_timer_args = {
        .callback = &lv_tick_task,
        .name = "periodic_gui"
    };
    esp_timer_handle_t periodic_timer;
    ESP_ERROR_CHECK(esp_timer_create(&periodic_timer_args, &periodic_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(periodic_timer, LV_TICK_PERIOD_MS * 1000));

    /* Create the demo application */
    create_demo_application();
    /* Force screen refresh */
    lv_refr_now(NULL);
    while (1) {
        /* Delay 1 tick (assumes FreeRTOS tick is 10ms */
        vTaskDelay(pdMS_TO_TICKS(10));

        /* Try to take the semaphore, call lvgl related function on success */
        if (pdTRUE == xSemaphoreTake(xGuiSemaphore, portMAX_DELAY)) {
            lv_task_handler();
            xSemaphoreGive(xGuiSemaphore);
       }
    }

    /* A task should NEVER return */
    free(buf1);
#ifndef CONFIG_LV_TFT_DISPLAY_MONOCHROME
    free(buf2);
#endif
    vTaskDelete(NULL);
}

static void create_demo_application(void)
{
    /* When using a monochrome display we only show "Hello World" centered on the
     * screen */
#if defined CONFIG_LV_TFT_DISPLAY_MONOCHROME || \
    defined CONFIG_LV_TFT_DISPLAY_CONTROLLER_ST7735S

    /* use a pretty small demo for monochrome displays */
    /* Get the current screen  */
    lv_obj_t * scr = lv_disp_get_scr_act(NULL);

    /*Create a Label on the currently active screen*/
    lv_obj_t * label1 =  lv_label_create(scr, NULL);

    /*Modify the Label's text*/
    lv_label_set_text(label1, "Hello\nworld");

    /* Align the Label to the center
     * NULL means align on parent (which is the screen now)
     * 0, 0 at the end means an x, y offset after alignment*/
    lv_obj_align(label1, NULL, LV_ALIGN_CENTER, 0, 0);
#else
    /* Otherwise we show the selected demo */

    #if defined CONFIG_LV_USE_DEMO_WIDGETS
        lv_demo_widgets();
    
    #elif defined CONFIG_LV_USE_DEMO_KEYPAD_AND_ENCODER
        lv_demo_keypad_encoder();
    #elif defined CONFIG_LV_USE_DEMO_BENCHMARK
        lv_demo_benchmark();
    #elif defined CONFIG_LV_USE_DEMO_STRESS
        lv_demo_stress();
    #else
        #error "No demo application selected."
    #endif
#endif
}

static void lv_tick_task(void *arg) {
    (void) arg;

    lv_tick_inc(LV_TICK_PERIOD_MS);
}
