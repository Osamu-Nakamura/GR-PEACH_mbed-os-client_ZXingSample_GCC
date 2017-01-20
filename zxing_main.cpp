
#include <vector>

#include "mbed.h"
#include "rtos.h"
#include "DisplayBace.h"
#include "ImageReaderSource.h"
#include "DisplayApp.h"
#include "AsciiFont.h"

#define VIDEO_CVBS             (0)                 /* Analog  Video Signal */
#define VIDEO_CMOS_CAMERA      (1)                 /* Digital Video Signal */
#define VIDEO_RGB888           (1)

/**** User Selection *********/
/** Camera setting **/
#define VIDEO_INPUT_METHOD     (VIDEO_CMOS_CAMERA) /* Select  VIDEO_CVBS or VIDEO_CMOS_CAMERA                       */
#define VIDEO_INPUT_FORMAT     (VIDEO_RGB888)      /* Select  VIDEO_RGB888                                          */
#define USE_VIDEO_CH           (0)                 /* Select  0 or 1            If selecting VIDEO_CMOS_CAMERA, should be 0.)               */
#define VIDEO_PAL              (0)                 /* Select  0(NTSC) or 1(PAL) If selecting VIDEO_CVBS, this parameter is not referenced.) */
/** LCD setting **/
#define LCD_ONOFF              (1)                 /* Select  0(without LCD) or 1(with LCD) */
#if LCD_ONOFF
#define LCD_TYPE               (0)                 /* Select  0(4.3inch) or 1(7.1inch) */
#endif
/*****************************/

#if LCD_ONOFF
/** LCD shield config **/
#if (LCD_TYPE == 0)
  #include "LCD_shield_config_4_3inch.h"
#else
  #include "LCD_shield_config_7_1inch.h"
#endif
#endif

/** Video and Grapics (GRAPHICS_LAYER_0) parameter **/
/* video input */
#if USE_VIDEO_CH == (0)
  #define VIDEO_INPUT_CH       (DisplayBase::VIDEO_INPUT_CHANNEL_0)
  #define VIDEO_INT_TYPE       (DisplayBase::INT_TYPE_S0_VFIELD)
#else
  #define VIDEO_INPUT_CH       (DisplayBase::VIDEO_INPUT_CHANNEL_1)
  #define VIDEO_INT_TYPE       (DisplayBase::INT_TYPE_S1_VFIELD)
#endif

/* NTSC or PAL */
#if VIDEO_PAL == 0
  #define COL_SYS              (DisplayBase::COL_SYS_NTSC_358)
#else
  #define COL_SYS              (DisplayBase::COL_SYS_PAL_443)
#endif

/* Video input */
#define VIDEO_FORMAT           (DisplayBase::VIDEO_FORMAT_RGB888)
#define GRAPHICS_FORMAT        (DisplayBase::GRAPHICS_FORMAT_RGB888)
#define WR_RD_WRSWA            (DisplayBase::WR_RD_WRSWA_32BIT)

/* The size of the video input */
#if ((LCD_ONOFF) && (LCD_TYPE == 0))
  #define VIDEO_PIXEL_HW                LCD_PIXEL_WIDTH
  #define VIDEO_PIXEL_VW                LCD_PIXEL_HEIGHT
#else
  #define VIDEO_PIXEL_HW                (640)  /* VGA */
  #define VIDEO_PIXEL_VW                (480)  /* VGA */
#endif

/*! Frame buffer stride: Frame buffer stride should be set to a multiple of 32 or 128
    in accordance with the frame buffer burst transfer mode. */
/* FRAME BUFFER Parameter GRAPHICS_LAYER_0 */
#define FRAME_BUFFER_NUM              (2u)
#define FRAME_BUFFER_BYTE_PER_PIXEL   (4u)
#define FRAME_BUFFER_STRIDE           (((VIDEO_PIXEL_HW * FRAME_BUFFER_BYTE_PER_PIXEL) + 31u) & ~31u)

#if LCD_ONOFF
#define VIDEO_PIXEL_HW_STR              (VIDEO_PIXEL_HW - 64)
#define VIDEO_PIXEL_VW_STR              (VIDEO_PIXEL_VW - 64)
#define FRAME_BUFFER_BYTE_PER_PIXEL_STR (2u)
#define FRAME_BUFFER_STRIDE_STR         (((VIDEO_PIXEL_HW_STR * FRAME_BUFFER_BYTE_PER_PIXEL_STR) + 31u) & ~31u)
#endif

static DisplayBase Display;
static DisplayApp  display_app;
static Timer decode_timer;
#if LCD_ONOFF
static DigitalOut  lcd_pwon(P7_15);
static DigitalOut  lcd_blon(P8_1);
static PwmOut      lcd_cntrst(P8_15);
#endif

/* 32 bytes aligned */
static uint8_t user_frame_buffer0[FRAME_BUFFER_STRIDE * VIDEO_PIXEL_VW]__attribute((aligned(32)));
static uint8_t user_frame_buffer1[FRAME_BUFFER_STRIDE * VIDEO_PIXEL_VW]__attribute((aligned(32)));
#if LCD_ONOFF
static uint8_t user_frame_buffer_string[FRAME_BUFFER_STRIDE_STR * VIDEO_PIXEL_VW_STR]__attribute((aligned(32)));
static AsciiFont ascii_font(user_frame_buffer_string, VIDEO_PIXEL_HW_STR, VIDEO_PIXEL_VW_STR, FRAME_BUFFER_STRIDE_STR, FRAME_BUFFER_BYTE_PER_PIXEL_STR);
static bool      string_draw;
#endif
static uint8_t * decode_buffer = user_frame_buffer0;
static uint8_t * FrameBufferTbl[FRAME_BUFFER_NUM] = {user_frame_buffer0, user_frame_buffer1};
static volatile int32_t vfield_count = 0;
static int write_buff_num = 0;
static bool graphics_init_end = false;
static int decode_wait_time = 0;
static void (*p_callback_func)(char * addr, int size);

/****** cache control ******/
static void dcache_clean(void * p_buf, uint32_t size) {
    uint32_t start_addr = (uint32_t)p_buf & 0xFFFFFFE0;
    uint32_t end_addr   = (uint32_t)p_buf + size;
    uint32_t addr;

    /* Data cache clean */
    for (addr = start_addr; addr < end_addr; addr += 0x20) {
        __v7_clean_dcache_mva((void *)addr);
    }
}

static void dcache_invalid(void * p_buf, uint32_t size){
    uint32_t start_addr = (uint32_t)p_buf & 0xFFFFFFE0;
    uint32_t end_addr   = (uint32_t)p_buf + size;
    uint32_t addr;

    /* Data cache invalid */
    for (addr = start_addr; addr < end_addr; addr += 0x20) {
        __v7_inv_dcache_mva((void *)addr);
    }
}

#if LCD_ONOFF
/****** LCD ******/
static void Init_LCD_Display(void) {
    DisplayBase::graphics_error_t error;
    DisplayBase::lcd_config_t lcd_config;
    PinName lvds_pin[8] = {
        /* data pin */
        P5_7, P5_6, P5_5, P5_4, P5_3, P5_2, P5_1, P5_0
    };

    lcd_pwon = 0;
    lcd_blon = 0;
    Thread::wait(100);
    lcd_pwon = 1;
    lcd_blon = 1;

    Display.Graphics_Lvds_Port_Init(lvds_pin, 8);

    /* Graphics initialization process */
    lcd_config = LcdCfgTbl_LCD_shield;
    error = Display.Graphics_init(&lcd_config);
    if (error != DisplayBase::GRAPHICS_OK) {
        printf("Line %d, error %d\n", __LINE__, error);
        mbed_die();
    }
    graphics_init_end = true;
}

static void Start_LCD_Display(uint8_t * p_buf) {
    DisplayBase::rect_t rect;

    rect.vs = 0;
    rect.vw = VIDEO_PIXEL_VW;
    rect.hs = 0;
    rect.hw = VIDEO_PIXEL_HW;
    Display.Graphics_Read_Setting(
        DisplayBase::GRAPHICS_LAYER_0,
        (void *)p_buf,
        FRAME_BUFFER_STRIDE,
        GRAPHICS_FORMAT,
        WR_RD_WRSWA,
        &rect
    );
    Display.Graphics_Start(DisplayBase::GRAPHICS_LAYER_0);
}
#endif

/****** Video ******/
static void IntCallbackFunc_Vfield(DisplayBase::int_type_t int_type) {
    /* Interrupt callback function for Vfield interruption */
    if (vfield_count > 0) {
        vfield_count--;
    }
}

static void Wait_Vfield(const int32_t wait_count) {
    /* Wait for the specified number of times Vsync occurs */
    vfield_count = wait_count;
    while (vfield_count > 0) {
        Thread::wait(1);
    }
}

static void Init_Video(void) {
    DisplayBase::graphics_error_t error;

    /* Graphics initialization process */
    if (graphics_init_end == false) {
        /* When not initializing LCD, this processing is needed. */
        error = Display.Graphics_init(NULL);
        if (error != DisplayBase::GRAPHICS_OK) {
            printf("Line %d, error %d\n", __LINE__, error);
            mbed_die();
        }
        graphics_init_end = true;
    }

#if VIDEO_INPUT_METHOD == VIDEO_CVBS
    error = Display.Graphics_Video_init( DisplayBase::INPUT_SEL_VDEC, NULL);
    if( error != DisplayBase::GRAPHICS_OK ) {
        printf("Line %d, error %d\n", __LINE__, error);
        mbed_die();
    }
#elif VIDEO_INPUT_METHOD == VIDEO_CMOS_CAMERA
    DisplayBase::video_ext_in_config_t ext_in_config;
    PinName cmos_camera_pin[11] = {
        /* data pin */
        P2_7, P2_6, P2_5, P2_4, P2_3, P2_2, P2_1, P2_0,
        /* control pin */
        P10_0,      /* DV0_CLK   */
        P1_0,       /* DV0_Vsync */
        P1_1        /* DV0_Hsync */
    };

    /* MT9V111 camera input config */
    ext_in_config.inp_format     = DisplayBase::VIDEO_EXTIN_FORMAT_BT601; /* BT601 8bit YCbCr format */
    ext_in_config.inp_pxd_edge   = DisplayBase::EDGE_RISING;              /* Clock edge select for capturing data          */
    ext_in_config.inp_vs_edge    = DisplayBase::EDGE_RISING;              /* Clock edge select for capturing Vsync signals */
    ext_in_config.inp_hs_edge    = DisplayBase::EDGE_RISING;              /* Clock edge select for capturing Hsync signals */
    ext_in_config.inp_endian_on  = DisplayBase::OFF;                      /* External input bit endian change on/off       */
    ext_in_config.inp_swap_on    = DisplayBase::OFF;                      /* External input B/R signal swap on/off         */
    ext_in_config.inp_vs_inv     = DisplayBase::SIG_POL_NOT_INVERTED;     /* External input DV_VSYNC inversion control     */
    ext_in_config.inp_hs_inv     = DisplayBase::SIG_POL_INVERTED;         /* External input DV_HSYNC inversion control     */
    ext_in_config.inp_f525_625   = DisplayBase::EXTIN_LINE_525;           /* Number of lines for BT.656 external input */
    ext_in_config.inp_h_pos      = DisplayBase::EXTIN_H_POS_CRYCBY;       /* Y/Cb/Y/Cr data string start timing to Hsync reference */
    ext_in_config.cap_vs_pos     = 6;                                     /* Capture start position from Vsync */
    ext_in_config.cap_hs_pos     = 150;                                   /* Capture start position form Hsync */
#if ((LCD_ONOFF) && (LCD_TYPE == 0))
    /* The same screen ratio as the screen ratio of the LCD. */
    ext_in_config.cap_width      = 640;                                   /* Capture width */
    ext_in_config.cap_height     = 363;                                   /* Capture height Max 468[line]
                                                                             Due to CMOS(MT9V111) output signal timing and VDC5 specification */
#else
    ext_in_config.cap_width      = 640;                                   /* Capture width */
    ext_in_config.cap_height     = 468;                                   /* Capture height Max 468[line]
                                                                             Due to CMOS(MT9V111) output signal timing and VDC5 specification */
#endif
    error = Display.Graphics_Video_init( DisplayBase::INPUT_SEL_EXT, &ext_in_config);
    if( error != DisplayBase::GRAPHICS_OK ) {
        printf("Line %d, error %d\n", __LINE__, error);
        mbed_die();
    }

    /* Camera input port setting */
    error = Display.Graphics_Dvinput_Port_Init(cmos_camera_pin, 11);
    if( error != DisplayBase::GRAPHICS_OK ) {
        printf("Line %d, error %d\n", __LINE__, error);
        mbed_die();
    }
#endif

    /* Interrupt callback function setting (Field end signal for recording function in scaler 0) */
    error = Display.Graphics_Irq_Handler_Set(VIDEO_INT_TYPE, 0, IntCallbackFunc_Vfield);
    if (error != DisplayBase::GRAPHICS_OK) {
        printf("Line %d, error %d\n", __LINE__, error);
        mbed_die();
    }
}

static void Start_Video(uint8_t * p_buf) {
    DisplayBase::graphics_error_t error;

    /* Video capture setting (progressive form fixed) */
    error = Display.Video_Write_Setting(
                VIDEO_INPUT_CH,
                COL_SYS,
                p_buf,
                FRAME_BUFFER_STRIDE,
                VIDEO_FORMAT,
                WR_RD_WRSWA,
                VIDEO_PIXEL_VW,
                VIDEO_PIXEL_HW
            );
    if (error != DisplayBase::GRAPHICS_OK) {
        printf("Line %d, error %d\n", __LINE__, error);
        mbed_die();
    }

    /* Video write process start */
    error = Display.Video_Start(VIDEO_INPUT_CH);
    if (error != DisplayBase::GRAPHICS_OK) {
        printf("Line %d, error %d\n", __LINE__, error);
        mbed_die();
    }

    /* Video write process stop */
    error = Display.Video_Stop(VIDEO_INPUT_CH);
    if (error != DisplayBase::GRAPHICS_OK) {
        printf("Line %d, error %d\n", __LINE__, error);
        mbed_die();
    }

    /* Video write process start */
    error = Display.Video_Start(VIDEO_INPUT_CH);
    if (error != DisplayBase::GRAPHICS_OK) {
        printf("Line %d, error %d\n", __LINE__, error);
        mbed_die();
    }
}

/****** zxing_init ******/
void zxing_init(void (*pfunc)(char * addr, int size)) {

#if LCD_ONOFF
    /* Initialization of LCD */
    Init_LCD_Display();    /* When using LCD, please call before than Init_Video(). */
#endif
    /* Initialization of Video */
    Init_Video();

    /* Initialization memory */
    for (int i = 0; i < FRAME_BUFFER_NUM; i++) {
        memset(FrameBufferTbl[i], 0, (FRAME_BUFFER_STRIDE * VIDEO_PIXEL_VW));
        dcache_clean(FrameBufferTbl[i], (FRAME_BUFFER_STRIDE * VIDEO_PIXEL_VW));
    }

    /* Start of Video */
    Start_Video(FrameBufferTbl[write_buff_num]);
    /* Wait for first video drawing */
    Wait_Vfield(2);
#if LCD_ONOFF
    DisplayBase::rect_t rect;

    /* The layer by which the touch panel location is drawn */
    ascii_font.Erase(0x00000000);  /* rrrrGBAR (r:Reserve G:Green B:Blue A:Alpha R:Red */
    dcache_clean(user_frame_buffer_string, sizeof(user_frame_buffer_string));
    rect.vs = 32;
    rect.vw = VIDEO_PIXEL_VW_STR;
    rect.hs = 32;
    rect.hw = VIDEO_PIXEL_HW_STR;
    Display.Graphics_Read_Setting(
        DisplayBase::GRAPHICS_LAYER_1,
        (void *)user_frame_buffer_string,
        FRAME_BUFFER_STRIDE_STR,
        DisplayBase::GRAPHICS_FORMAT_ARGB4444,
        DisplayBase::WR_RD_WRSWA_32_16BIT,
        &rect
    );
    Display.Graphics_Start(DisplayBase::GRAPHICS_LAYER_1);
    string_draw = false;
    /* Start of LCD */
    Start_LCD_Display(FrameBufferTbl[write_buff_num]);
    /* Backlight on */
    Thread::wait(200);
    lcd_cntrst.write(1.0);
#endif
    p_callback_func = pfunc;
    decode_timer.reset();
    decode_timer.start();
}

/****** zxing_main ******/
int zxing_loop() {
    DisplayBase::graphics_error_t error;
    int decode_result = -1;
    vector<Ref<Result> > results;

    decode_buffer = FrameBufferTbl[write_buff_num];
    write_buff_num++;
    if (write_buff_num >= FRAME_BUFFER_NUM) {
        write_buff_num = 0;
    }
    /* Change video buffer */
    error = Display.Video_Write_Change(VIDEO_INPUT_CH, FrameBufferTbl[write_buff_num], FRAME_BUFFER_STRIDE);
    if (error != DisplayBase::GRAPHICS_OK) {
        printf("Line %d, error %d\n", __LINE__, error);
        mbed_die();
    }
    Wait_Vfield(2);
#if LCD_ONOFF
    /* Change LCD buffer */
    Display.Graphics_Read_Change(DisplayBase::GRAPHICS_LAYER_0, (void *)FrameBufferTbl[write_buff_num]);
#endif
    dcache_invalid(decode_buffer, (FRAME_BUFFER_STRIDE * VIDEO_PIXEL_VW));
    /* Decode barcode image */
    if (decode_timer.read_ms() >= decode_wait_time) {
        decode_timer.reset();
        decode_result = ex_decode(decode_buffer, (FRAME_BUFFER_STRIDE * VIDEO_PIXEL_VW), VIDEO_PIXEL_HW, VIDEO_PIXEL_VW, &results);
        if (decode_result == 0) {
            char ** decode_str;
            int     size;

            decode_str = (char **)&(results[0]->getText()->getText());
            size = strlen(*decode_str);
            if (p_callback_func != NULL) {
                p_callback_func(*decode_str, size);
            }
#if LCD_ONOFF
            /* Drow string */
            ascii_font.Erase(0x00000090);  /* rrrrGBAR (r:Reserve G:Green B:Blue A:Alpha R:Red */
            int rest_size = strlen(*decode_str);
            int draw_idx = 0;
            int draw_size;
            int draw_line = 0;

            while (rest_size > 0) {
                draw_size = ascii_font.DrawStr(*decode_str + draw_idx, 6, 5 + (18 * draw_line), 0x0000ffff, 2);
                if (draw_size <= 0) {
                    break;
                }
                rest_size -= draw_size;
                draw_idx += draw_size;
                draw_line++;
            }
           
            dcache_clean(user_frame_buffer_string, sizeof(user_frame_buffer_string));
            string_draw = true;
#endif
            decode_wait_time = 500;
        } else {
#if LCD_ONOFF
            if (string_draw != false) {
                /* Clear string */
                ascii_font.Erase(0x00000000);  /* rrrrGBAR (r:Reserve G:Green B:Blue A:Alpha R:Red */
                dcache_clean(user_frame_buffer_string, sizeof(user_frame_buffer_string));
                string_draw = false;
            }
#endif
            decode_wait_time = 10;
        }
    }
    display_app.SendRgb888(decode_buffer, VIDEO_PIXEL_HW, VIDEO_PIXEL_VW);

    return decode_result;
}
