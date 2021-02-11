

#include "stdio.h"
#include "stdlib.h"
#include "string.h"


//#include "ext/types-compat.h"
//#include "ext/videodev2.h"
//#include "../v4l2_nv_extensions.h"

#include "gst_buffer_info_meta.h"

static gboolean gst_buffer_info_meta_init(GstMeta *meta, gpointer params, GstBuffer *buffer);
static gboolean gst_buffer_info_meta_transform(GstBuffer *transbuf, GstMeta *meta, GstBuffer *buffer, GQuark type, gpointer data);
static void gst_buffer_info_meta_free(GstMeta *meta, GstBuffer *buffer);

// ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
//
GstBufferInfo* empty()
{
    static GstBufferInfo info;

    //memset ((void *) &info.m_enc_mv_metadata, 0, sizeof (info.m_enc_mv_metadata));

    return &info;
}

// ---------------------------------------------------------------------------------------------------------------------------------------------------
//
void AllocateMyMetaData( GstBufferInfo *pBufferInfo, v4l2_ctrl_videoenc_outputbuf_metadata_MV *p_meta_MV, int nInfoCount )
{
    if (pBufferInfo != NULL)
        {
        //memset ((void *) &pBufferInfo->m_enc_mv_metadata, 0, sizeof(pBufferInfo->m_enc_mv_metadata));

        if (p_meta_MV != NULL)
            {
            
            if (p_meta_MV->bufSize != 0)
                {
                #ifdef D_USE_META_STATIC
                    int nMaxSize = sizeof(pBufferInfo->m_enc_mv_metadata.rec_mv_info);
                    if (nMaxSize >= p_meta_MV->bufSize)   // buffer must be sufficient
                        {
                        pBufferInfo->m_enc_mv_metadata.bufSize = p_meta_MV->bufSize;
                        memcpy( &pBufferInfo->m_enc_mv_metadata.rec_mv_info[0], p_meta_MV->pMVInfo, pBufferInfo->m_enc_mv_metadata.bufSize );
                        pBufferInfo->m_enc_mv_metadata.m_nInfoCount = nInfoCount;
                        }
                    else
                        {
                        g_print ("AllocateMyMetaData too large - nMaxSize=%d x %d\n", nMaxSize, p_meta_MV->bufSize );
                        }
                #else
                    pBufferInfo->m_enc_mv_metadata.pMVInfo = malloc( p_meta_MV->bufSize );
                    if (pBufferInfo->m_enc_mv_metadata.pMVInfo != NULL)
                        {
                        pBufferInfo->m_enc_mv_metadata.bufSize = p_meta_MV->bufSize;
                        memcpy( pBufferInfo->m_enc_mv_metadata.pMVInfo, p_meta_MV->pMVInfo, pBufferInfo->m_enc_mv_metadata.bufSize );
                        }
                #endif
                }
            }
        }  
}

// ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
//
// Register metadata type and returns Gtype
// https://gstreamer.freedesktop.org/data/doc/gstreamer/head/gstreamer/html/gstreamer-GstMeta.html#gst-meta-api-type-register
GType gst_buffer_info_meta_api_get_type(void)
{
    static const gchar *tags[] = {NULL};
    static volatile GType type;
    if (g_once_init_enter (&type)) {
        GType _type = gst_meta_api_type_register("GstBufferInfoMetaAPI", tags);
        g_once_init_leave(&type, _type);
        g_print ("GstBufferInfoMetaAPI register 1\n");
    }
    return type;
}

// GstMetaInfo provides info for specific metadata implementation
// https://gstreamer.freedesktop.org/data/doc/gstreamer/head/gstreamer/html/gstreamer-GstMeta.html#GstMetaInfo

// ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
//
const GstMetaInfo *gst_buffer_info_meta_get_info(void)
{
    static const GstMetaInfo *gst_buffer_info_meta_info = NULL;
 
    if (g_once_init_enter (&gst_buffer_info_meta_info)) {
        // Explanation of fields
        // https://gstreamer.freedesktop.org/documentation/design/meta.html#gstmeta1
        const GstMetaInfo *meta = gst_meta_register (GST_BUFFER_INFO_META_API_TYPE, /* api type */
                                                     "GstBufferInfoMeta",           /* implementation type */
                                                     sizeof (GstBufferInfoMeta),    /* size of the structure */
                                                     gst_buffer_info_meta_init,
                                                     //(GstMetaFreeFunction) NULL,
                                                    gst_buffer_info_meta_free,
                                                     gst_buffer_info_meta_transform);
        g_once_init_leave (&gst_buffer_info_meta_info, meta);
    
        //g_print ("gst_meta_register\n");
    }
    return gst_buffer_info_meta_info;
}
 
// ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
//
// Meta init function
// 4-th field in GstMetaInfo
static gboolean gst_buffer_info_meta_init(GstMeta *meta, gpointer params, GstBuffer *buffer)
{
    GstBufferInfoMeta *gst_buffer_info_meta = (GstBufferInfoMeta*)meta;     

    memset ((void *) &gst_buffer_info_meta->info.m_enc_mv_metadata, 0, sizeof (gst_buffer_info_meta->info.m_enc_mv_metadata));

    //g_print ("gst_buffer_info_meta_init\n");

    return TRUE;
}

static void gst_buffer_info_meta_free( GstMeta *meta, GstBuffer *buffer)
{
    //g_print ("F");

    #ifndef D_USE_META_STATIC
        GstBufferInfoMeta *gst_buffer_info_meta = (GstBufferInfoMeta *)meta;
        if (gst_buffer_info_meta != NULL)
            {
            free( gst_buffer_info_meta->info.m_enc_mv_metadata.pMVInfo );
            gst_buffer_info_meta->info.m_enc_mv_metadata.pMVInfo = NULL;
            }
    #endif
}
 
// ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
//
// Meta transform function
// 5-th field in GstMetaInfo
// https://gstreamer.freedesktop.org/data/doc/gstreamer/head/gstreamer/html/gstreamer-GstMeta.html#GstMetaTransformFunction
static gboolean gst_buffer_info_meta_transform(GstBuffer *transbuf, GstMeta *meta, GstBuffer *buffer,
                                               GQuark type, gpointer data)
{
    GstBufferInfoMeta *gst_buffer_info_meta = (GstBufferInfoMeta *)meta;
    gst_buffer_add_buffer_info_meta(transbuf, &(gst_buffer_info_meta->info) );

    //g_print ("gst_buffer_info_meta_transform 1\n");

    return TRUE;
}


// ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
//
GstBufferInfoMeta* gst_buffer_add_buffer_info_meta( GstBuffer *buffer, GstBufferInfo* buffer_info )
{   
    GstBufferInfoMeta *gst_buffer_info_meta = NULL;

    //g_print ("gst_buffer_add_buffer_info_meta 1\n");

    // check that gst_buffer valid
    g_return_val_if_fail(GST_IS_BUFFER(buffer), NULL);

    // check that gst_buffer writable
    if ( !gst_buffer_is_writable(buffer))
        return gst_buffer_info_meta;

    // https://gstreamer.freedesktop.org/data/doc/gstreamer/head/gstreamer/html/GstBuffer.html#gst-buffer-add-meta
    gst_buffer_info_meta = (GstBufferInfoMeta *) gst_buffer_add_meta (buffer, GST_BUFFER_INFO_META_INFO, NULL);

    #ifdef D_USE_META_STATIC
        gst_buffer_info_meta->info.m_enc_mv_metadata.bufSize        = buffer_info->m_enc_mv_metadata.bufSize;
        gst_buffer_info_meta->info.m_enc_mv_metadata.m_nInfoCount   = buffer_info->m_enc_mv_metadata.m_nInfoCount;

        memcpy( &gst_buffer_info_meta->info.m_enc_mv_metadata.rec_mv_info[0], &buffer_info->m_enc_mv_metadata.rec_mv_info[0], gst_buffer_info_meta->info.m_enc_mv_metadata.bufSize );
    #else
        if (buffer_info != NULL)
            {
            if (buffer_info->m_enc_mv_metadata.bufSize != 0)
                {
                gst_buffer_info_meta->info.m_enc_mv_metadata.pMVInfo = malloc( buffer_info->m_enc_mv_metadata.bufSize );
                if (gst_buffer_info_meta->info.m_enc_mv_metadata.pMVInfo != NULL)
                    {
                    gst_buffer_info_meta->info.m_enc_mv_metadata.bufSize = buffer_info->m_enc_mv_metadata.bufSize;
                    memcpy( gst_buffer_info_meta->info.m_enc_mv_metadata.pMVInfo, buffer_info->m_enc_mv_metadata.pMVInfo, gst_buffer_info_meta->info.m_enc_mv_metadata.bufSize );
                    }
                }
            }
    #endif

    return gst_buffer_info_meta;
}

// ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
//
// Removes metadata (GstBufferInfo) from buffer
gboolean gst_buffer_remove_buffer_info_meta(GstBuffer *buffer)
{
    g_return_val_if_fail(GST_IS_BUFFER(buffer), FALSE );

    //g_print ("gst_buffer_remove_buffer_info_meta\n");

    GstBufferInfoMeta* meta = (GstBufferInfoMeta*)gst_buffer_get_meta((buffer), GST_BUFFER_INFO_META_API_TYPE);

    if (meta == NULL)
        return TRUE;
    
    if ( !gst_buffer_is_writable(buffer))
        return FALSE;

    // https://gstreamer.freedesktop.org/data/doc/gstreamer/head/gstreamer/html/GstBuffer.html#gst-buffer-remove-meta
    return gst_buffer_remove_meta(buffer, &meta->meta);
}

// ---------------------------------------------------------------------------------------------------------------------------------------------------
