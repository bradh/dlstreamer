#!/bin/bash
# ==============================================================================
# Copyright (C) 2020-2022 Intel Corporation
#
# SPDX-License-Identifier: MIT
# ==============================================================================

set -e

# Command-line parameters
INPUT=${1:-https://github.com/intel-iot-devkit/sample-videos/raw/master/person-bicycle-car-detection.mp4} # Input file or URL
DETECTION_INTERVAL=${2:-3}     # Object detection interval: 1 means detection every frame, 2 means detection every second frame, etc.
DEVICE=${3:-AUTO}              # Device for decode and inference in OpenVINO(TM) format, examples: AUTO, CPU, GPU, GPU.0
OUTPUT=${4:-display-async}     # Output type, valid values: display, display-async, fps, json, display-and-json
TRACKING_TYPE=${5:-short-term-imageless} # Object tracking type, valid values: short-term-imageless, zero-term, zero-term-imageless

# Models
MODEL_1=person-vehicle-bike-detection-2004
MODEL_2=person-attributes-recognition-crossroad-0230
MODEL_3=vehicle-attributes-recognition-barrier-0039

# Reclassify interval (run classification every 10th frame)
RECLASSIFY_INTERVAL=10

if [[ $INPUT == "/dev/video"* ]]; then
  SOURCE_ELEMENT="v4l2src device=${INPUT}"
elif [[ $INPUT == *"://"* ]]; then
  SOURCE_ELEMENT="urisourcebin buffer-size=4096 uri=${INPUT}"
else
  SOURCE_ELEMENT="filesrc location=${INPUT}"
fi

if [[ $DEVICE == "CPU" ]]; then
  DECODE_ELEMENT="decodebin force-sw-decoders=true"
elif [[ $DEVICE == "GPU" ]]; then
  DECODE_ELEMENT="decodebin ! video/x-raw\(memory:VASurface\)"
else
  DECODE_ELEMENT="decodebin"
fi

if [[ $OUTPUT == "display" ]]; then
  SINK_ELEMENT="gvawatermark ! videoconvert ! gvafpscounter ! autovideosink"
elif [[ $OUTPUT == "display-async" ]]; then
  SINK_ELEMENT="gvawatermark ! videoconvert ! gvafpscounter ! autovideosink sync=false"
elif [[ $OUTPUT == "fps" ]]; then
  SINK_ELEMENT="gvafpscounter ! fakesink async=false "
elif [[ $OUTPUT == "json" ]]; then
  SINK_ELEMENT="gvametaconvert ! gvametapublish file-format=json-lines file-path=output.json ! fakesink async=false"
elif [[ $OUTPUT == "display-and-json" ]]; then
  SINK_ELEMENT="gvawatermark ! gvametaconvert ! gvametapublish file-format=json-lines file-path=output.json ! videoconvert ! gvafpscounter ! autovideosink sync=false"
else
  echo Error wrong value for OUTPUT parameter
  echo Valid values: "display" - render to screen, "fps" - print FPS, "json" - write to output.json, "display-and-json" - render to screen and write to output.json
  exit
fi

PROC_PATH() {
    echo $(dirname "$0")/model_proc/$1.json
}

DETECTION_MODEL=${MODELS_PATH}/intel/${MODEL_1}/FP32/${MODEL_1}.xml
PERSON_CLASSIFICATION_MODEL=${MODELS_PATH}/intel/${MODEL_2}/FP32/${MODEL_2}.xml
VEHICLE_CLASSIFICATION_MODEL=${MODELS_PATH}/intel/${MODEL_3}/FP32/${MODEL_3}.xml

DETECTION_MODEL_PROC=$(PROC_PATH $MODEL_1)
PERSON_CLASSIFICATION_MODEL_PROC=$(PROC_PATH $MODEL_2)
VEHICLE_CLASSIFICATION_MODEL_PROC=$(PROC_PATH $MODEL_3)

PIPELINE="gst-launch-1.0 \
  ${SOURCE_ELEMENT} ! ${DECODE_ELEMENT} ! queue ! \
  gvadetect model=$DETECTION_MODEL \
            model-proc=$DETECTION_MODEL_PROC \
            inference-interval=${DETECTION_INTERVAL} \
            threshold=0.4 \
            device=${DEVICE} ! \
  queue ! \
  gvatrack tracking-type=${TRACKING_TYPE} ! \
  queue ! \
  gvaclassify model=$PERSON_CLASSIFICATION_MODEL \
              model-proc=$PERSON_CLASSIFICATION_MODEL_PROC \
              reclassify-interval=${RECLASSIFY_INTERVAL} \
              device=${DEVICE} \
              object-class=person ! \
  queue ! \
  gvaclassify model=$VEHICLE_CLASSIFICATION_MODEL \
              model-proc=$VEHICLE_CLASSIFICATION_MODEL_PROC \
              reclassify-interval=${RECLASSIFY_INTERVAL} \
              device=${DEVICE} \
              object-class=vehicle ! \
  queue ! \
  $SINK_ELEMENT"

echo ${PIPELINE}
eval $PIPELINE