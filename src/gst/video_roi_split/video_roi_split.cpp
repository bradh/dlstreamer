/*******************************************************************************
 * Copyright (C) 2021-2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

#include "video_roi_split.h"

#include "dlstreamer/gst/buffer.h"
#include "dlstreamer/metadata.h"
#include "metadata/gva_tensor_meta.h"
#include "region_of_interest.h"
#include <meta/gva_buffer_flags.hpp>

#define PROP_USE_CROP_META_DEFAULT FALSE

namespace {
GstFlowReturn send_gap_event(GstPad *pad, GstBuffer *buf) {
    auto gap_event = gst_event_new_gap(GST_BUFFER_PTS(buf), GST_BUFFER_DURATION(buf));
    return gst_pad_push_event(pad, gap_event) ? GST_BASE_TRANSFORM_FLOW_DROPPED : GST_FLOW_ERROR;
}
} // namespace

GST_DEBUG_CATEGORY(roi_split_debug_category);
#define GST_CAT_DEFAULT roi_split_debug_category

G_DEFINE_TYPE(RoiSplit, roi_split, GST_TYPE_BASE_TRANSFORM);

enum { PROP_0, PROP_OBJECT_CLASS };

static void roi_split_init(RoiSplit *self) {
    GST_DEBUG_OBJECT(self, "%s", __FUNCTION__);
    self->object_classes = NULL;
    self->use_crop_meta = PROP_USE_CROP_META_DEFAULT;
}

static void roi_split_set_property(GObject *object, guint prop_id, const GValue *value, GParamSpec *pspec) {
    RoiSplit *self = ROI_SPLIT(object);
    GST_DEBUG_OBJECT(self, "%s", __FUNCTION__);

    switch (prop_id) {
    case PROP_OBJECT_CLASS:
        if (!self->object_classes)
            self->object_classes = new std::vector<std::string>;
        *self->object_classes = dlstreamer::split_string(g_value_get_string(value));
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
        break;
    }
}

static void roi_split_get_property(GObject *object, guint prop_id, GValue *value, GParamSpec *pspec) {
    RoiSplit *self = ROI_SPLIT(object);
    GST_DEBUG_OBJECT(self, "%s", __FUNCTION__);

    switch (prop_id) {
    case PROP_OBJECT_CLASS:
        if (self->object_classes) {
            auto str = dlstreamer::join_strings(self->object_classes->begin(), self->object_classes->end());
            g_value_set_string(value, str.c_str());
        }
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
        break;
    }
}

static void roi_split_finalize(GObject *object) {
    RoiSplit *self = ROI_SPLIT(object);
    GST_DEBUG_OBJECT(self, "%s", __FUNCTION__);

    if (self->object_classes)
        delete self->object_classes;

    G_OBJECT_CLASS(roi_split_parent_class)->finalize(object);
}

static gboolean roi_split_sink_event(GstBaseTransform *base, GstEvent *event) {
    RoiSplit *self = ROI_SPLIT(base);
    GST_DEBUG_OBJECT(self, "%s", __FUNCTION__);

    return GST_BASE_TRANSFORM_CLASS(roi_split_parent_class)->sink_event(base, event);
}

static GstFlowReturn roi_split_transform_ip(GstBaseTransform *base, GstBuffer *buf) {
    RoiSplit *self = ROI_SPLIT(base);
    GST_DEBUG_OBJECT(self, "%s", __FUNCTION__);

    // Filter ROIs by object class
    GstVideoRegionOfInterestMeta *roi_meta;
    std::vector<GstVideoRegionOfInterestMeta *> rois;
    gpointer state = nullptr;
    while ((roi_meta = ((GstVideoRegionOfInterestMeta *)gst_buffer_iterate_meta_filtered(
                buf, &state, GST_VIDEO_REGION_OF_INTEREST_META_API_TYPE)))) {
        if (self->object_classes && roi_meta->roi_type) {
            auto &classes = *self->object_classes;
            std::string name = g_quark_to_string(roi_meta->roi_type);
            if (std::find(classes.begin(), classes.end(), name) == classes.end())
                continue;
        }
        rois.push_back(roi_meta);
    }

    if (rois.empty()) {
        GST_DEBUG_OBJECT(self, "No ROI meta. Push GAP event: ts=%" GST_TIME_FORMAT, GST_TIME_ARGS(GST_BUFFER_PTS(buf)));
        return send_gap_event(base->srcpad, buf);
    }

    // Create new buffers per ROI and push
    GstMetaTransformCopy copy_data = {.region = FALSE, .offset = 0, .size = static_cast<guint>(-1)};
    for (size_t i = 0; i < rois.size(); i++) {
        roi_meta = rois[i];

        // Create separate buffers from buf for each ROI meta
        GstBuffer *roi_buf = gst_buffer_new();

        // Copy everything except meta
        if (!gst_buffer_copy_into(roi_buf, buf,
                                  static_cast<GstBufferCopyFlags>(GST_BUFFER_COPY_ALL & ~GST_BUFFER_COPY_META), 0,
                                  static_cast<gsize>(-1))) {
            GST_ERROR_OBJECT(self, "Failed to copy data from input buffer into roi buffer");
            gst_buffer_unref(roi_buf);
            return GST_FLOW_ERROR;
        }

        // TODO: remove copying ROIMeta after migration to VideoCropMeta completed
        if (!roi_meta->meta.info->transform_func(roi_buf, GST_META_CAST(roi_meta), buf, _gst_meta_transform_copy,
                                                 &copy_data)) {
            GST_ERROR_OBJECT(self, "Failed to copy meta to roi buffer: %s", g_type_name(roi_meta->meta.info->api));
            gst_buffer_unref(roi_buf);
            return GST_FLOW_ERROR;
        }

        // attach VideoCropMeta
        auto crop_meta = gst_buffer_add_video_crop_meta(roi_buf);
        crop_meta->x = roi_meta->x;
        crop_meta->y = roi_meta->y;
        crop_meta->width = roi_meta->w;
        crop_meta->height = roi_meta->h;

        // attach SourceIdentifierMetadata
        const GstMetaInfo *meta_info = gst_meta_get_info(GVA_TENSOR_META_IMPL_NAME);
        GstGVATensorMeta *meta = (GstGVATensorMeta *)gst_buffer_add_meta(roi_buf, meta_info, NULL);
        gst_structure_set_name(meta->data, dlstreamer::SourceIdentifierMetadata::name);
        GVA::RegionOfInterest gva_roi(roi_meta);
        gst_structure_set(meta->data, dlstreamer::SourceIdentifierMetadata::key::roi_id, G_TYPE_INT,
                          gva_roi.region_id(), dlstreamer::SourceIdentifierMetadata::key::object_id, G_TYPE_INT,
                          gva_roi.object_id(), dlstreamer::SourceIdentifierMetadata::key::pts, G_TYPE_POINTER,
                          static_cast<intptr_t>(GST_BUFFER_PTS(buf)), NULL);

        if (i == rois.size() - 1) { // set custom flag on last buffer
            gst_buffer_set_flags(roi_buf, static_cast<GstBufferFlags>(GVA_BUFFER_FLAG_LAST_ROI_ON_FRAME));
        }

        // push
        GST_DEBUG_OBJECT(self, "Push ROI buffer: id=%u ts=%" GST_TIME_FORMAT, roi_meta->id,
                         GST_TIME_ARGS(GST_BUFFER_PTS(roi_buf)));
        if (gst_pad_push(GST_BASE_TRANSFORM_SRC_PAD(base), roi_buf) != GST_FLOW_OK) {
            GST_ERROR_OBJECT(self, "Failed to push ROI buffer");
            return GST_FLOW_ERROR;
        }
    }

    return GST_BASE_TRANSFORM_FLOW_DROPPED;
}

static void roi_split_class_init(RoiSplitClass *klass) {
    auto gobject_class = G_OBJECT_CLASS(klass);
    gobject_class->set_property = roi_split_set_property;
    gobject_class->get_property = roi_split_get_property;
    gobject_class->finalize = roi_split_finalize;

    auto base_transform_class = GST_BASE_TRANSFORM_CLASS(klass);
    base_transform_class->sink_event = roi_split_sink_event;
    base_transform_class->transform_ip = roi_split_transform_ip;

    auto element_class = GST_ELEMENT_CLASS(klass);
    gst_element_class_set_static_metadata(element_class, ROI_SPLIT_NAME, "application", ROI_SPLIT_DESCRIPTION,
                                          "Intel Corporation");

    gst_element_class_add_pad_template(element_class,
                                       gst_pad_template_new("src", GST_PAD_SRC, GST_PAD_ALWAYS, GST_CAPS_ANY));
    gst_element_class_add_pad_template(element_class,
                                       gst_pad_template_new("sink", GST_PAD_SINK, GST_PAD_ALWAYS, GST_CAPS_ANY));

    g_object_class_install_property(
        gobject_class, PROP_OBJECT_CLASS,
        g_param_spec_string("object-class", "object-class",
                            "Filter ROI list by object class(es) (comma separated list if multiple). Output only ROIs "
                            "with specified object class(es)",
                            "", static_cast<GParamFlags>(G_PARAM_WRITABLE | G_PARAM_STATIC_STRINGS)));
}
