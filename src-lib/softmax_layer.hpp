#pragma once

#include "darknet_internal.hpp"

void softmax_array(float *input, int n, float temp, float *output);
Darknet::Layer make_softmax_layer(int batch, int inputs, int groups);
void forward_softmax_layer(Darknet::Layer & l, Darknet::NetworkState state);
void backward_softmax_layer(Darknet::Layer & l, Darknet::NetworkState state);

#ifdef DARKNET_GPU
void pull_softmax_layer_output(const Darknet::Layer & l);
void forward_softmax_layer_gpu(Darknet::Layer & l, Darknet::NetworkState state);
void backward_softmax_layer_gpu(Darknet::Layer & l, Darknet::NetworkState state);
#endif

//-----------------------

Darknet::Layer make_contrastive_layer(int batch, int w, int h, int n, int classes, int inputs, Darknet::Layer *yolo_layer);
void forward_contrastive_layer(Darknet::Layer & l, Darknet::NetworkState state);
void backward_contrastive_layer(Darknet::Layer & l, Darknet::NetworkState state);

#ifdef DARKNET_GPU
void pull_contrastive_layer_output(const Darknet::Layer & l);
void push_contrastive_layer_output(const Darknet::Layer & l);
void forward_contrastive_layer_gpu(Darknet::Layer & l, Darknet::NetworkState state);
void backward_contrastive_layer_gpu(Darknet::Layer & l, Darknet::NetworkState state);
#endif
