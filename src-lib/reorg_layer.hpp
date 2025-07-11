#pragma once

#include "darknet_internal.hpp"

Darknet::Layer make_reorg_layer(int batch, int w, int h, int c, int stride, int reverse);
void resize_reorg_layer(Darknet::Layer *l, int w, int h);
void forward_reorg_layer(Darknet::Layer & l, Darknet::NetworkState state);
void backward_reorg_layer(Darknet::Layer & l, Darknet::NetworkState state);

#ifdef DARKNET_GPU
void forward_reorg_layer_gpu(Darknet::Layer & l, Darknet::NetworkState state);
void backward_reorg_layer_gpu(Darknet::Layer & l, Darknet::NetworkState state);
#endif
