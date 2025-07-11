#pragma once

#include "darknet_internal.hpp"

Darknet::Layer make_scale_channels_layer(int batch, int index, int w, int h, int c, int w2, int h2, int c2, int scale_wh);
void forward_scale_channels_layer(Darknet::Layer & l, Darknet::NetworkState state);
void backward_scale_channels_layer(Darknet::Layer & l, Darknet::NetworkState state);
void resize_scale_channels_layer(Darknet::Layer *l, Darknet::Network * net);

#ifdef DARKNET_GPU
void forward_scale_channels_layer_gpu(Darknet::Layer & l, Darknet::NetworkState state);
void backward_scale_channels_layer_gpu(Darknet::Layer & l, Darknet::NetworkState state);
#endif
