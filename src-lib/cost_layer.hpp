#pragma once

#include "darknet_internal.hpp"

COST_TYPE get_cost_type(char *s);
const char *get_cost_string(COST_TYPE a);
Darknet::Layer make_cost_layer(int batch, int inputs, COST_TYPE cost_type, float scale);
void forward_cost_layer(Darknet::Layer & l, Darknet::NetworkState state);
void backward_cost_layer(Darknet::Layer & l, Darknet::NetworkState state);
void resize_cost_layer(Darknet::Layer *l, int inputs);

#ifdef DARKNET_GPU
void forward_cost_layer_gpu(Darknet::Layer & l, Darknet::NetworkState state);
void backward_cost_layer_gpu(Darknet::Layer & l, Darknet::NetworkState state);
#endif
