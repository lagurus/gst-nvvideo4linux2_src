#include <gst/gst.h>
#include <gst/app/gstappsink.h>

 
#include <opencv2/opencv.hpp>
#include <opencv2/highgui/highgui.hpp>
#include <opencv2/core/ocl.hpp>
#include <opencv2/core/utils/filesystem.hpp>

#include <stdlib.h>
#include <termio.h>
#include <unistd.h>
#include <queue>

using namespace cv;


// ----------------------------------------------------------------------------------------------------------------------------------------------------------------------
//
GstFlowReturn new_preroll(GstAppSink *appsink, gpointer data)
{
  g_print ("Got basic_preroll!\n");
  return GST_FLOW_OK;
}

GstFlowReturn new_preroll2(GstAppSink *appsink, gpointer data)
{
  g_print ("Got h264_preroll!\n");
  return GST_FLOW_OK;
}


// --------------------------------------------------------------------------------------------------------------------------------------------

#define D_USE_META_STATIC   1         // no malloc, static prealocation

typedef struct _GstBufferInfoMeta  GstBufferInfoMeta;
typedef struct _GstBufferInfo      GstBufferInfo    ;

// folowings is define in #include "../v4l2_nv_extensions.h" but need to add a lot of stuff to
// fullfill compilers needs
//
typedef struct MVInfo_ {
    /** Number of pixels the macro block moved in horizontal direction. */
    gint32 mv_x   : 16;
    /** Number of pixels the macro block moved in vertical direction. */
    gint32 mv_y   : 14;
    /** Temporal hints used by hardware for Motion Estimation. */
    guint32 weight : 2;
} MVInfo;

/**
 * Holds the motion vector parameters for one complete frame.
 */
typedef struct metadata_MV_
{
    /** Size of the pMVInfo buffer, in bytes. */
    guint32 bufSize;
    /** Pointer to the buffer containing the motion vectors. */
    #ifdef D_USE_META_STATIC    // no malloc, static prealocation
        int        m_nInfoCount;
        MVInfo  rec_mv_info[12000];
    #else
        MVInfo  *pMVInfo;
    #endif

} metadata_MV;

struct _GstBufferInfo
{
    //long              m_lCounter;
    metadata_MV       m_enc_mv_metadata;
};

struct _GstBufferInfoMeta
{
    // Required as it is base structure for metadata
    // https://gstreamer.freedesktop.org/data/doc/gstreamer/head/gstreamer/html/gstreamer-GstMeta.html
    GstMeta meta;  

    // Custom fields
    GstBufferInfo info;
};  

// ----------------------------------------------------------------------------------------------------------------------------------------------------------------------
//
struct sink_user_data
{
    int m_nWidth;
    int m_nHeight;
    int m_nMotionWidth;
    int m_nMotionHeight;
    int m_nDetectWidth;
    int m_nDetectHeight;

    int m_nRTSPStream;      // need v4l2rtspserver - run with ./v4l2rtspserver  -F30 /dev/video5 -Q 2

    int m_nDisplay2;

    int m_nDisplay2_X ;
    int m_nDisplay2_Y;
    int m_nDisplay2_Width;
    int m_nDisplay2_Height;

    bool m_bNeedData;
};

// ----------------------------------------------------------------------------------------------------------------------------------------------------------------------
//

struct sFrameAndMotionVector
{
    Mat                     m_Mat;
    metadata_MV      m_enc_mv_metadata;
};

// TODO: use synchronized deque
std::queue<Mat>                 frameQueue;
std::queue<metadata_MV>  metaQueue;

pthread_mutex_t 		m_thread_lock;

static int nFileCounter = 0;


// ----------------------------------------------------------------------------------------------------------------------------------------------------------------------
//
GstFlowReturn new_sample(GstAppSink *appsink, gpointer data)
{
    sink_user_data      *p_rec_user_data = (sink_user_data*) data;

    static int framecount = 0;
    static std::chrono::steady_clock::time_point T_start= std::chrono::steady_clock::now();

    framecount++;

    GstSample *sample = gst_app_sink_pull_sample(appsink);
    GstCaps *caps = gst_sample_get_caps(sample);
    GstBuffer *buffer = gst_sample_get_buffer(sample);
    const GstStructure *info = gst_sample_get_info(sample);

    // ---- Read frame and convert to opencv format ---------------

    GstMapInfo map;
    gst_buffer_map (buffer, &map, GST_MAP_READ);

    // convert gstreamer data to OpenCV Mat, you could actually
    // resolve height / width from caps...
    //Mat frame(Size(320, 240), CV_8UC3, (char*)map.data, Mat::AUTO_STEP);
    int frameSize = map.size;
    
    Mat frame(Size(p_rec_user_data->m_nDetectWidth, p_rec_user_data->m_nDetectHeight), CV_8UC4, (char*)map.data, Mat::AUTO_STEP);
    
    Mat edges;
    cvtColor(frame, edges, cv::COLOR_RGBA2BGR);

    // TODO: synchronize this....
    pthread_mutex_lock( &m_thread_lock );
        //frameQueue.push_back(frame);
        frameQueue.push(edges);
    pthread_mutex_unlock( &m_thread_lock );
    
    gst_buffer_unmap(buffer, &map);
    
    // ------------------------------------------------------------

    //int flags = GST_BUFFER_FLAGS( buffer );
    //g_print( "GST: %04d %04X Size=%d %s \n", framecount, flags, frameSize, (flags & GST_BUFFER_FLAG_DELTA_UNIT) ? " " : "*" );
    
    // ------------------------------------------------------------

    // print dot every 30 frames
    if (framecount%30 == 0)
        {
        std::chrono::steady_clock::time_point T_end = std::chrono::steady_clock::now();
        g_print (".");
        g_print ("%lldms ", static_cast<long long int>(std::chrono::duration_cast <std::chrono::milliseconds> (T_end - T_start).count()) );
        T_start = T_end;
        }

    // show caps on first frame
    if (framecount == 1)
        {
        g_print ("Basic: %s\n", gst_caps_to_string(caps));
        }

    gst_sample_unref (sample);
    return GST_FLOW_OK;
}

// ----------------------------------------------------------------------------------------------------------------------------------------------------------------------
//
GstFlowReturn new_h264_sample(GstAppSink *appsink, gpointer data)
{
    sink_user_data      *p_rec_user_data = (sink_user_data*) data;

    static int framecount2 = 0;
    static std::chrono::steady_clock::time_point T_start2= std::chrono::steady_clock::now();

    framecount2++;

    GstSample *sample = gst_app_sink_pull_sample(appsink);
    GstCaps *caps = gst_sample_get_caps(sample);
    GstBuffer *buffer = gst_sample_get_buffer(sample);
    const GstStructure *info = gst_sample_get_info(sample);

    if (1)
        {
        GstBufferInfoMeta* meta = (GstBufferInfoMeta*) gst_buffer_get_meta(buffer, g_type_from_name("GstBufferInfoMetaAPI"));
        if (meta != NULL)
            {
            if (meta->info.m_enc_mv_metadata.bufSize > 0)
                {
                #ifdef D_USE_META_STATIC    // no malloc, static prealocation
                
                    //g_print ("Meta info2 %d \n", meta->info.m_enc_mv_metadata.m_nInfoCount );
                
                    metadata_MV       rec_enc_mv_metadata;
                
                    rec_enc_mv_metadata.bufSize = meta->info.m_enc_mv_metadata.bufSize;
                    rec_enc_mv_metadata.m_nInfoCount = meta->info.m_enc_mv_metadata.m_nInfoCount;
                
                    memcpy( &rec_enc_mv_metadata.rec_mv_info[0], &meta->info.m_enc_mv_metadata.rec_mv_info[0], rec_enc_mv_metadata.bufSize );
                
                    metaQueue.push(rec_enc_mv_metadata);
                
                #else
                    metadata_MV       rec_enc_mv_metadata;
                  
                    rec_enc_mv_metadata.pMVInfo = (MVInfo*) malloc( meta->info.m_enc_mv_metadata.bufSize + 10 );
                    if (rec_enc_mv_metadata.pMVInfo != NULL)
                        {
                        rec_enc_mv_metadata.bufSize = meta->info.m_enc_mv_metadata.bufSize;

                        memset( rec_enc_mv_metadata.pMVInfo, 0, rec_enc_mv_metadata.bufSize + 10 );
                        memcpy( rec_enc_mv_metadata.pMVInfo, &meta->info.m_enc_mv_metadata.pMVInfo, rec_enc_mv_metadata.bufSize );
                    
                        metaQueue.push(rec_enc_mv_metadata);
                        }
                    #endif
              
              } // if (meta->info.m_enc_mv_metadata.bufSize > 0)
          }
      }

    // ------------------------------------------------------------

    //int flags = GST_BUFFER_FLAGS( buffer );
    //g_print( "GST: %04d %04X Size=%d %s \n", framecount, flags, frameSize, (flags & GST_BUFFER_FLAG_DELTA_UNIT) ? " " : "*" );

    // print dot every 30 frames
    if (framecount2%30 == 0)
        {
        std::chrono::steady_clock::time_point T_end = std::chrono::steady_clock::now();
        g_print ("+");
        g_print ("%lldms ", static_cast<long long int>(std::chrono::duration_cast <std::chrono::milliseconds> (T_end - T_start2).count()) );
        T_start2 = T_end;
        }

    // show caps on first frame
    if (framecount2 == 1)
        {
        g_print ("H264:%s\n", gst_caps_to_string(caps));
        }

    gst_sample_unref (sample);
    return GST_FLOW_OK;
}

// ------------------------------------------------------------------------------------------------------------------------
//
// onNeedData
void appsrc_onNeedData( GstElement* pipeline, guint size, gpointer user_data )
{
    //g_print( "appsrc requesting data (%u bytes)\n", size);
	
	if( !user_data )
		return;

    sink_user_data      *p_rec_user_data = (sink_user_data*) user_data;
    
	p_rec_user_data->m_bNeedData  = true;
}
 
// ------------------------------------------------------------------------------------------------------------------------
//
// onEnoughData
void appsrc_onEnoughData( GstElement* pipeline, gpointer user_data )
{
    //g_print( "appsrc signalling enough data\n");

	if( !user_data )
		return;

	sink_user_data      *p_rec_user_data = (sink_user_data*) user_data;
    
	p_rec_user_data->m_bNeedData  = false;
}

// ----------------------------------------------------------------------------------------------------------------------------------------------------------------------
//
static gboolean my_bus_callback (GstBus *bus, GstMessage *message, gpointer data)
{
  g_print ("GST: Got %s message\n", GST_MESSAGE_TYPE_NAME (message));
  switch (GST_MESSAGE_TYPE (message)) {
    case GST_MESSAGE_ERROR: {
      GError *err;
      gchar *debug;

      gst_message_parse_error (message, &err, &debug);
      g_print ("GST: Error: %s\n", err->message);
      g_error_free (err);
      g_free (debug);
      break;
    }
    case GST_MESSAGE_EOS:
      /* end-of-stream */
      break;
    default:
      /* unhandled message */
      break;
  }
  /* we want to be notified again the next time there is a message
   * on the bus, so returning TRUE (FALSE means we want to stop watching
   * for messages on the bus and our callback should not be called again)
   */
  return TRUE;
}

// ----------------------------------------------------------------------------------------------------------------------------------------------------------------------
//
int main (int argc, char *argv[])
{
    GError *error = NULL;

    gst_init (&argc, &argv);

    int nShowWindow = 2;

    if (argc > 1)
        nShowWindow = atoi( argv[1] );

    if (pthread_mutex_init(&m_thread_lock, NULL) != 0)
		{
	    fprintf(stderr, "m_thread_lock init failed\n");
		}

    sink_user_data      my_user_data;

    my_user_data.m_nWidth = 1280;
    my_user_data.m_nHeight = 720;
    my_user_data.m_nDetectWidth = 640;
    my_user_data.m_nDetectHeight = 480;
    
    my_user_data.m_nRTSPStream = 0;     // need v4l2rtspserver - run with ./v4l2rtspserver  -F30 /dev/video5 -Q 2
    
    my_user_data.m_nDisplay2 = 1;
    my_user_data.m_nDisplay2_X = 960;
    my_user_data.m_nDisplay2_Y = 0;
    my_user_data.m_nDisplay2_Width = 960;
    my_user_data.m_nDisplay2_Height = 540;

    // For H.264, nvenc provides one motion vector per 16x16 block(Macroblock).
    // For H.265, nvenc provides one motion vector per 32x32 block(Coded Tree Block).

    my_user_data.m_nMotionWidth					= ((my_user_data.m_nWidth + 15) / 16);
    my_user_data.m_nMotionHeight 					= (my_user_data.m_nHeight / 16) ;
    
    float fX = (my_user_data.m_nDetectWidth * 1.0) / my_user_data.m_nWidth;
	float fY = (my_user_data.m_nDetectHeight * 1.0)/ my_user_data.m_nHeight;

    g_print ("%dx%d = %dx%d - fX,fY=%f,%f - size=%d\n",
                        my_user_data.m_nWidth, my_user_data.m_nHeight, my_user_data.m_nMotionWidth, my_user_data.m_nMotionHeight,
                        fX, fY,
                        my_user_data.m_nMotionWidth * my_user_data.m_nMotionHeight  );
    
    gchar *descr = g_strdup_printf( "videotestsrc pattern=ball num-buffers=30000 ! video/x-raw,width=(int)%d,height=(int)%d,format=(string)I420,framerate=(fraction)30/1 ! nvvidconv ! video/x-raw(memory:NVMM), format=(string)I420 ! tee name=tp  "
    //gchar *descr = g_strdup_printf( "nvarguscamerasrc ! video/x-raw(memory:NVMM), width=(int)%d, height=(int)%d, format=(string)NV12,framerate=(fraction)30/1 ! tee name=tp "
                                            //" tp. ! queue ! nvv4l2h264enc EnableMVBufferMeta=1 name=h264_encode ! identity name=shit ! video/x-h264,stream-format=(string)byte-stream,alignment=(string)au ! appsink name=sink_h264 sync=true"
                                            //" tp. ! queue ! nvvidconv ! video/x-raw,format=RGBA,width=(int)%d,height=(int)%d ! appsink name=sink_basic sync=true "
                                        
                                        " tp. ! queue ! nvv4l2h264enc EnableMVBufferMeta=1 name=h264_encode ! identity name=shit ! video/x-h264,stream-format=(string)byte-stream,alignment=(string)au ! appsink name=sink_h264 sync=true"
                                        " tp. ! queue ! nvvidconv ! video/x-raw,format=RGBA,width=(int)%d,height=(int)%d ! appsink name=sink_basic sync=true "
                                        ,
                                        my_user_data.m_nWidth, my_user_data.m_nHeight,
                                        my_user_data.m_nDetectWidth, my_user_data.m_nDetectHeight

                                    );
                                    
    // ------------------------------------------------------------------------------------------------
    
    gchar *descr_appsrc_stream3 = g_strdup_printf(  " " );
    gchar *descr_appsrc_disp = g_strdup_printf(  " " );
    
    if (my_user_data.m_nRTSPStream)      // rtsp stream
        {
        descr_appsrc_stream3 = g_strdup_printf(  " tp_disp2. ! queue !  nvv4l2h264enc bitrate=%d insert-sps-pps=true name=stream3b_h264 ! h264parse config-interval=-1 ! video/x-h264,stream-format=(string)byte-stream ! v4l2sink device=/dev/video5 ",    // run with ./v4l2rtspserver -v -F30 /dev/video4
                                                                500000 );
        }
    
    if (my_user_data.m_nDisplay2)     // live display
        {
        descr_appsrc_disp = g_strdup_printf(  " tp_disp2. ! queue ! nvvidconv ! video/x-raw(memory:NVMM), width=(int)%d, height=(int)%d ! nvoverlaysink name=disp_basic overlay=2 overlay-depth=1 overlay-x=%d overlay-y=%d overlay-w=%d overlay-h=%d ",
									my_user_data.m_nDisplay2_Width, my_user_data.m_nDisplay2_Height,
									my_user_data.m_nDisplay2_X, my_user_data.m_nDisplay2_Y, my_user_data.m_nDisplay2_Width, my_user_data.m_nDisplay2_Height );
        }
    
    gchar *descr_appcsrc_out = g_strdup_printf(    "appsrc name=my_src is-live=true do-timestamp=true format=3 ! video/x-raw,width=(int)%d,height=(int)%d,framerate=%d/1,format=RGBA ! nvvidconv ! video/x-raw(memory:NVMM),format=I420  ! tee name=tp_disp2 "
                                                                            "%s "   // descr_appsrc_stream3
                                                                            "%s ",   // descr_appsrc_disp
    
                                                                            my_user_data.m_nDetectWidth, my_user_data.m_nDetectHeight, 30 /*m_p_rec_settings->m_nCaptureFPS*/,

                                                                            descr_appsrc_stream3,
                                                                            descr_appsrc_disp
    
                                                                            );
        
    // ------------------------------------------------------------------------------------------------
    
    g_print( "\nmain: %s\n", descr );
    g_print( "\nappsrc_out: %s\n", descr_appcsrc_out );
    
    // ------------------------------------------------------------------------------------------------

  GstElement *pipeline = gst_parse_launch (descr, &error);

    if (error != NULL)
        {
        g_print ("could not construct pipeline: %s\n", error->message);
        g_error_free (error);
        exit (-1);
        }
    
    GstElement  *m_pipeline_appsrc_out;
    m_pipeline_appsrc_out = gst_parse_launch (descr_appcsrc_out, &error);
    if (error != NULL)
        {
        g_print ("could not construct m_pipeline_appsrc_out: %s\n", error->message);
        g_error_free (error);
        exit (-1);
        }
    
    // ------------------------------------------------------------------------------------------------

    /* get sink */
    GstElement *sink_basic = gst_bin_get_by_name (GST_BIN (pipeline), "sink_basic");

    gst_app_sink_set_emit_signals((GstAppSink*)sink_basic, true);
    gst_app_sink_set_drop((GstAppSink*)sink_basic, true);
    gst_app_sink_set_max_buffers((GstAppSink*)sink_basic, 1);
    GstAppSinkCallbacks callbacks = { NULL, new_preroll, new_sample };
    gst_app_sink_set_callbacks (GST_APP_SINK(sink_basic), &callbacks, &my_user_data, NULL);

    GstElement *sink_h264 = gst_bin_get_by_name (GST_BIN (pipeline), "sink_h264");
    gst_app_sink_set_emit_signals((GstAppSink*)sink_h264, true);
    gst_app_sink_set_drop((GstAppSink*)sink_h264, true);
    gst_app_sink_set_max_buffers((GstAppSink*)sink_h264, 1);
    GstAppSinkCallbacks callbacks2 = { NULL, new_preroll2, new_h264_sample };
    gst_app_sink_set_callbacks (GST_APP_SINK(sink_h264), &callbacks2, &my_user_data, NULL);

    // --------------------------------------------------------------------------------------------------------------------------------------

    GstElement *m_appsrc = NULL;
    
    if (m_pipeline_appsrc_out)
        {
        m_appsrc = gst_bin_get_by_name((GstBin *)m_pipeline_appsrc_out,  "my_src" );
        if (m_appsrc != NULL)
            {
            gst_util_set_object_arg (G_OBJECT (m_appsrc), "format", "time");
            
            g_signal_connect(m_appsrc, "need-data", G_CALLBACK(appsrc_onNeedData), &my_user_data );
            g_signal_connect(m_appsrc, "enough-data", G_CALLBACK(appsrc_onEnoughData), &my_user_data );
            
            // retrieve pipeline bus
            //m_bus_appsrc = gst_pipeline_get_bus(GST_PIPELINE(m_pipeline_appsrc_out) );
            //bus_watch_id = gst_bus_add_watch (m_bus_appsrc, my_bus_callback, gpointer("my_src") );
            }
        }
    
    // ------------------------------------------------------------------------------------------------

    GstBus *bus;
    guint bus_watch_id;
    bus = gst_pipeline_get_bus (GST_PIPELINE (pipeline));
    bus_watch_id = gst_bus_add_watch (bus, my_bus_callback, NULL);
    gst_object_unref (bus);

    gst_element_set_state (GST_ELEMENT (pipeline), GST_STATE_PLAYING);
    
    if (m_pipeline_appsrc_out)
        gst_element_set_state (GST_ELEMENT (m_pipeline_appsrc_out), GST_STATE_PLAYING);

    // ------------------------------------------------------------------------------------------------

    if (nShowWindow >= 4)
        {
        cv::namedWindow("edges",WINDOW_NORMAL);
        cv::resizeWindow("edges", 640, 480);
        }
    
    char lpstrFile[256];    
    
    // ------------------------------------------------------------------------------------------------
    
    int nMaxX = 0;
    int nMaxY = 0;
    int nMinX = 0;
    int nMinY = 0;
    
    metadata_MV     rec_enc_mv_metadata;
    
    rec_enc_mv_metadata.m_nInfoCount = 0;

    while(1)
        {
        g_main_iteration(false);
        
        // ------------------------------------------------------------------------------------------------
        
        if (nShowWindow)
            {
              if (metaQueue.size() > 0)
                {
                rec_enc_mv_metadata = metaQueue.front();
                
                metaQueue.pop();
                
                g_print("M" );
                
                }   // if (metaQueue.size() > 0)
            }   // if (nShowWindow)
            
        // ------------------------------------------------------------------------------------------------

        if (nShowWindow)
            {
            //g_print("f" );
            
            Mat frame;
            
            pthread_mutex_lock( &m_thread_lock );
            
            if (frameQueue.size() > 0)
                {
                // this lags pretty badly even when grabbing frames from webcam
                frame = frameQueue.front();
                //frameQueue.pop_front();
                frameQueue.pop();
                }
            
            pthread_mutex_unlock( &m_thread_lock );
                
            if (!frame.empty())
                {
                g_print("F" );
                
                int nWrite = 0;
                
                if (nShowWindow >= 2)
                    {
                    if (rec_enc_mv_metadata.m_nInfoCount > 0)   // we already have motion vectors
                        {
                        for (int y = 0; y < my_user_data.m_nMotionHeight; y++)
                          {
                          for (int x = 0; x < my_user_data.m_nMotionWidth; x++)
                              {
                              int nPos = (x + y * my_user_data.m_nMotionWidth);
                              if (nPos >= 0 && nPos < 12000)
                                  {
                                    MVInfo *pInfo = &rec_enc_mv_metadata.rec_mv_info[nPos];
                                    if (pInfo->mv_x != 0 || pInfo->mv_y != 0)
                                      {
                                        int chX = ((pInfo->mv_x / 16.0) );
                                        int chY = ((pInfo->mv_y / 16.0) );
                              
                                          
                                          int x1 = fX * (x * 16 + 8);
                                          int y1 = fY * (y * 16 + 8);
                                          int x2 = fX * (x * 16 + 8 + chX);
                                          int y2 = fY * (y * 16 + 8 + chY);
                                          
                                          //g_print("%d,%d->%d,%d", x1,y1,x2,y2 );
                                          
                                          //cv::line( frame, cv::Point(x1,y1), cv::Point(x2,y2), Scalar(0, 255, 0), 1, LINE_4);
                                          cv::arrowedLine( frame, cv::Point(x2,y2), cv::Point(x1,y1), Scalar(0, 255, 0), 1, LINE_4 );
                                      }
                                  }
                              }   // for (int x = 0; x < my_user_data.m_nMotionWidth; x++)
                          }   // for (int y = 0; y < my_user_data.m_nMotionHeight; y++)
                        }   // if (rec_enc_mv_metadata.m_nInfoCount > 0) 

                    if (nWrite && nShowWindow >= 2)
                        {
                        sprintf( lpstrFile, "test%04d.jpg", nFileCounter++ );
                        cv::imwrite( lpstrFile, frame );
                        
                        }

                    if (nShowWindow >= 3)
                        {
                        if (nShowWindow == 4)
                            cv::imshow("edges", frame );
                        
                        }
                    }   // if (nShowWindow >= 2)
                
                // ---------------------------------------------------------------------------------------------------------------------------
                
                if (m_pipeline_appsrc_out)
                    {
                    //g_print( "+" );
                    
                    if(my_user_data.m_bNeedData)
                        {
                        g_print( "*" );
                        
                        Mat mat_conv;
                        cvtColor( frame, mat_conv, cv::COLOR_BGR2RGBA );
                        //cvtColor( frame, mat_conv, cv::COLOR_BGR2YUV_I420 );
                        
                        int size = mat_conv.total() * mat_conv.elemSize();
                        
                        
                        // allocate gstreamer buffer memory
                        GstBuffer* gstBuffer = gst_buffer_new_allocate(NULL, size, NULL);
                        
                        // map the buffer for write access
                        GstMapInfo map; 

                        if( gst_buffer_map(gstBuffer, &map, GST_MAP_WRITE) ) 
                            { 
                            if( map.size != size )
                                {
                                g_print( "gstEncoder -- gst_buffer_map() size mismatch, got %d bytes, expected %d bytes", (int) map.size, size);
                                gst_buffer_unref(gstBuffer);
                                return 0;
                                }
                            
                            memcpy(map.data, mat_conv.data, size);
                            gst_buffer_unmap(gstBuffer, &map); 
                            } 
                        else
                            {
                            g_print( "gstEncoder -- failed to map gstreamer buffer memory (%d bytes)", size);
                            gst_buffer_unref(gstBuffer);
                            return 0;
                            }
                        
                        // queue buffer to gstreamer
                        GstFlowReturn ret;	
                        g_signal_emit_by_name(m_appsrc, "push-buffer", gstBuffer, &ret);
                        gst_buffer_unref(gstBuffer);

                        if( ret != 0 )
                            g_print( "gstEncoder -- appsrc pushed buffer abnormally (result %u)\n", ret);
                        
                        //checkMsgBus();
                        
                        }   // if(my_user_data.m_bNeedData)
                    }   // if (m_pipeline_appsrc_out)
            
                // ---------------------------------------------------------------------------------------------------------------------------

                }   // if (!frame.empty())
            
            }   // if (nShowWindow)

        // ------------------------------------------------------------------------------------------------
        
        if (nShowWindow >= 3)
            cv::waitKey(10);  // cause of 100% CPU in headless only
        else
            usleep(3000);
        
        }   // while(1)
    
    if (m_pipeline_appsrc_out)
        {
        gst_element_send_event( m_pipeline_appsrc_out,gst_event_new_eos() );

        gst_element_set_state (GST_ELEMENT (m_pipeline_appsrc_out), GST_STATE_NULL);
        gst_object_unref (GST_OBJECT (m_pipeline_appsrc_out));
        }

    gst_element_set_state (GST_ELEMENT (pipeline), GST_STATE_NULL);
    gst_object_unref (GST_OBJECT (pipeline));


    pthread_mutex_destroy( &m_thread_lock );
    
    // ------------------------------------------------------------------------------------------------

    return 0;
}

