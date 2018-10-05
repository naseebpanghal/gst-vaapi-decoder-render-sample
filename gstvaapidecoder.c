//gcc gstvaapidecoder.c -I/usr/include/ffmpeg/ -I/usr/include/gstreamer-1.0/ -I/usr/include/glib-2.0/ -I/usr/lib64/glib-2.0/include/ -I/usr/lib64/gstreamer-1.0/include/ -lavcodec -lavutil -lavformat -lgstreamer-1.0 -lgstvaapi-x11-1.4 -lgstvaapi-1.4 -o gstvaapidecoder
//gcc gstvaapidecoder.c `pkg-config --cflags --libs gstreamer-vaapi-1.0` `pkg-config --libs gstreamer-vaapi-x11-1.0` `pkg-config --cflags --libs gtk+-2.0` -I/usr/include/ffmpeg/ -lavcodec -lavutil -lavformat -o gstvaapidecoder -g

#include "libavformat/avformat.h"
#include <gst/gst.h>
#include <gst/vaapi/gstvaapidisplay_x11.h>
#include <gst/vaapi/gstvaapidecoder_h264.h>
#include <gst/vaapi/gstvaapidecoder_jpeg.h>
#include <gtk/gtk.h>
#include <gdk/gdkx.h>

//#define RAW_DUMP
//#define RESET_DECODER

#define WIDTH   1920
#define HEIGHT  1080

GstVaapiDecoder *decoder = NULL;
VADisplay disID;
GtkWidget *window;
int start = 0;

int init_decoder()
{
    int ret = 0;

    if (decoder != NULL)
    {
        gst_vaapi_object_unref(decoder);
    }
    GstCaps *caps = gst_caps_from_string("video/x-h264");
    //GstCaps *caps = gst_caps_from_string("image/jpeg");
/*
    gst_caps_set_simple(caps, "width", G_TYPE_INT, 1920,
      "height", G_TYPE_INT, 1080,
      NULL);
*/
    GstVaapiDisplay* display = gst_vaapi_display_x11_new(NULL);
    if(!display)
    {
        printf("display is NULL\n");
        ret = -1;
        goto end;
    }
    disID = gst_vaapi_display_get_display(display);
    decoder = gst_vaapi_decoder_h264_new(display, caps);
    //decoder = gst_vaapi_decoder_jpeg_new(display, caps);
    if(!decoder)
    {
        printf("decoder is NULL\n");
        ret = -1;
        goto end;
    }
end:
    gst_caps_unref(caps);
    if(display)
        gst_vaapi_display_unref(display);
    return ret;
}

int check_frame_type(guint8* buff, guint32 length)
{
    int ret = 0;
    guint8* temp = buff;
    guint32 len = length;
    guint8 NALType = 0;
    gboolean bStopProcessing = FALSE;
    gboolean bResetDecoder = FALSE;
    guint32 i = 0;
    do
    {
        guint32 zeroCount = 0;
        gboolean bStartCodeFound = FALSE;

        len = len-(temp-buff);
        for (i = 0 ; i < len; i++, temp++)
        {
            switch(temp[0])
            {
                case 0x00:
                    zeroCount++;
                    break;
                case 0x01:
                    if (zeroCount >= 2)
                    {
                        bStartCodeFound = TRUE; // startCodeSize = ((zeroCount + 1) < 4) ? zeroCount + 1 : 4;
                        temp++; // remove 0x01 symbol
                    }
                    zeroCount = 0;
                    break;
                default:
                    zeroCount = 0;
                    break;
            }

            if (bStartCodeFound)
                break;
        }

        if (!bStartCodeFound)
            break;

        NALType = temp[0] & 0x1F;
        switch(NALType)
        {
            case 1:         // Coded slice of a non-IDR picture
                //printf("Non-IDR frame detected\n");
                bStopProcessing = TRUE;
                break;
            case 5:          // Coded slice of an IDR picture
                //printf("IDR frame detected\n");
                bResetDecoder = TRUE;
                bStopProcessing = TRUE;
                break;
            case 6:         // SEI (Supplemental Enhancement Information)
                //printf("SEI frame detected\n");
                temp += 4;  // skip some bytes in SEI data
                break;
            case 7:
                //printf("SPS frame detected\n");
                temp += 5;  // skip some bytes in SPS data
                break;
            case 8:
                //printf("PPS frame detected\n");
                temp += 4;  // skip 4 bytes of PPS data
                break;
            case 9:         // AUD (Access Unit Delimiter)
                //printf("AUD frame detected\n");
                temp += 2;  // skip 2 bytes of AUD data
                break;
            default:
                break;
        }
    }while (!bStopProcessing);

    if (bResetDecoder)
    {
        printf("Reseting Decoder\n");
        ret = init_decoder();
    }
    return ret;
}

int video_decode_example(const char *input_filename)
{
    int ret = 0;
    int video_stream;
    AVPacket pkt;
    AVFormatContext *fmt_ctx = NULL;
    int result;
    int end_of_stream = 0;
    gboolean success;
#ifdef RAW_DUMP
    FILE *efp = fopen("decoder.yuv", "w+");
#endif

    result = avformat_open_input(&fmt_ctx, input_filename, NULL, NULL);
    if (result < 0) 
    {
        av_log(NULL, AV_LOG_ERROR, "Can't open file\n");
        return result;
    }

    avformat_find_stream_info(fmt_ctx, NULL);
    int width = fmt_ctx->streams[0]->codec->width;
    int height = fmt_ctx->streams[0]->codec->height;
    int format = fmt_ctx->streams[0]->codec->pix_fmt;
    //printf("%d %d %d\n", width, height, format);

    av_init_packet(&pkt);


    ret = init_decoder();
    if(ret == -1)
    {
        printf("init_decoder failed\n");
        goto end;
    }

    int frame_count = 0;
    do
    {
        if (!end_of_stream)
            if (av_read_frame(fmt_ctx, &pkt) < 0)
                end_of_stream = 1;
        if (end_of_stream)
        {
            pkt.data = NULL;
            pkt.size = 0;
        }
        if (!end_of_stream)
        {
            frame_count++;
#ifdef RESET_DECODER
            ret = check_frame_type(pkt.data, pkt.size);
#endif
            if(ret == 0)
            {
                //printf("Frame No.[%d] size[%d]\n", frame_count, pkt.size);
                GstBuffer *buffer = gst_buffer_new_wrapped_full(GST_MEMORY_FLAG_READONLY,
                        (guchar *)pkt.data, pkt.size, 0, pkt.size, NULL, NULL);
                if(buffer)
                {
                    success = gst_vaapi_decoder_put_buffer(decoder, buffer);
                    if(success)
                    {
                        success = gst_vaapi_decoder_put_buffer(decoder, NULL);
                        if(success)
                        {
                            gst_buffer_unref(buffer);

                            GstVaapiSurfaceProxy *proxy ;
                            GstVaapiDecoderStatus status = gst_vaapi_decoder_get_surface(decoder, &proxy);
                            if(!status)
                            {
                                //printf("proxy is OK ERROR[%d]\n", status);
                                GstVaapiSurface *surface = gst_vaapi_surface_proxy_get_surface(proxy);
                                gst_vaapi_surface_sync(surface);
#ifdef RAW_DUMP
                                GstVaapiImage *image = gst_vaapi_surface_derive_image(surface);

                                VAImage va_image;
                                gst_vaapi_image_get_image (image, &va_image);
                                gst_vaapi_image_map(image);

                                if(efp)
                                {
#if 1
                                    fwrite(gst_vaapi_image_get_plane (image, 0), 1, (va_image.pitches[0] * (height)), efp);
                                    fwrite(gst_vaapi_image_get_plane (image, 1), 1, (va_image.pitches[1] * (height/2)), efp);
#else
                                    for(int i = 0; i < height; i++)
                                    {
                                        fwrite(gst_vaapi_image_get_plane (image, 0) + (va_image.pitches[0]*i), 1, width, efp);
                                    }
                                    for(int i = 0; i < height/2; i++)
                                    {
                                        fwrite(gst_vaapi_image_get_plane (image, 1) + (va_image.pitches[1]*i), 1, width, efp);
                                    }
#endif
                                }

                                gst_vaapi_image_unmap(image);
                                gst_vaapi_object_unref(image);
#endif
                                gst_vaapi_surface_proxy_unref(proxy);
                                vaPutSurface(disID, gst_vaapi_surface_get_id(surface), GDK_WINDOW_XID(gtk_widget_get_window(window)), 0, 0, width, height, 0, 0, WIDTH, HEIGHT, NULL, 0, VA_FRAME_PICTURE);
                            }
                            else
                            {
                                printf("proxy is NULL Frame No[%d] ERROR[%d]\n", frame_count, status);
                            }
                        }
                        else
                        {
                            gst_buffer_unref(buffer);
                            printf("gst_vaapi_decoder_put_buffer(with NULL) failed\n");
                        }
                    }
                    else
                    {
                        gst_buffer_unref(buffer);
                        printf("gst_vaapi_decoder_put_buffer failed\n");
                    }
                }
                else
                {
                    printf("GstBuffer Creation Failed\n");
                }
            }
            else
            {
                printf("check_frame_type failed\n");
            }

        }
        usleep(33000);
    } while (!end_of_stream);

end:
#ifdef RAW_DUMP
    if(efp)
        fclose(efp);
#endif
    if(decoder)
        gst_vaapi_object_unref(decoder);
    av_packet_unref(&pkt);
    avformat_close_input(&fmt_ctx);
    gtk_main_quit();
    return 0;
}

static gboolean da_expose (GtkWidget *da, GdkEvent *event, gpointer data)
{
    printf("*************** 1  *************\n");
    start = 1;
    if(start == 1)
    {
        start = 0;
        if (video_decode_example(data) != 0)
            return 1;
    }
    return 1;
}

int main(int argc, char **argv)
{
    if (argc < 2)
    {
        av_log(NULL, AV_LOG_ERROR, "Incorrect input\n");
        return 1;
    }

    gtk_init (&argc , &argv);
    av_register_all();
    gst_init(NULL, NULL);

    GtkWidget *canvas;
    window = gtk_window_new (GTK_WINDOW_TOPLEVEL);
    gtk_widget_set_size_request (window, WIDTH, HEIGHT);

    gtk_window_move(GTK_WINDOW(window), 0, 0);

    canvas = gtk_drawing_area_new ();
    gtk_container_add (GTK_CONTAINER (window), canvas);
    g_signal_connect (canvas, "expose_event", (GCallback) da_expose, argv[1]);
    gtk_widget_show_all (window);

    gtk_main();

    return 0;
}
