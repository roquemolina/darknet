#include "darknet_internal.hpp"


namespace
{
	static auto & cfg_and_state = Darknet::CfgAndState::get();
}


COST_TYPE get_cost_type(char *s)
{
	TAT(TATPARMS);

	if (strcmp(s, "sse")==0) return SSE;
	if (strcmp(s, "masked")==0) return MASKED;
	if (strcmp(s, "smooth")==0) return SMOOTH;

	*cfg_and_state.output << "Couldn't find cost type " << s << ", going with SSE" << std::endl;

	return SSE;
}

const char *get_cost_string(COST_TYPE a)
{
	TAT(TATPARMS);

	switch(a){
		case SSE:
			return "sse";
		case MASKED:
			return "masked";
		case SMOOTH:
			return "smooth";
		default:
			return "sse";
	}
}

Darknet::Layer make_cost_layer(int batch, int inputs, COST_TYPE cost_type, float scale)
{
	TAT(TATPARMS);

	*cfg_and_state.output << "cost                                           " << inputs << std::endl;

	Darknet::Layer l = { (Darknet::ELayerType)0 };
	l.type = Darknet::ELayerType::COST;

	l.scale = scale;
	l.batch = batch;
	l.inputs = inputs;
	l.outputs = inputs;
	l.cost_type = cost_type;
	l.delta = (float*)xcalloc(inputs * batch, sizeof(float));
	l.output = (float*)xcalloc(inputs * batch, sizeof(float));
	l.cost = (float*)xcalloc(1, sizeof(float));

	l.forward = forward_cost_layer;
	l.backward = backward_cost_layer;
	#ifdef DARKNET_GPU
	l.forward_gpu = forward_cost_layer_gpu;
	l.backward_gpu = backward_cost_layer_gpu;

	l.delta_gpu = cuda_make_array(l.delta, inputs*batch);
	l.output_gpu = cuda_make_array(l.output, inputs*batch);
	#endif
	return l;
}

void resize_cost_layer(Darknet::Layer *l, int inputs)
{
	TAT(TATPARMS);

	l->inputs = inputs;
	l->outputs = inputs;
	l->delta = (float*)xrealloc(l->delta, inputs * l->batch * sizeof(float));
	l->output = (float*)xrealloc(l->output, inputs * l->batch * sizeof(float));
#ifdef DARKNET_GPU
	cuda_free(l->delta_gpu);
	cuda_free(l->output_gpu);
	l->delta_gpu = cuda_make_array(l->delta, inputs*l->batch);
	l->output_gpu = cuda_make_array(l->output, inputs*l->batch);
#endif
}

void forward_cost_layer(Darknet::Layer & l, Darknet::NetworkState state)
{
	TAT(TATPARMS);

	if (!state.truth) return;
	if(l.cost_type == MASKED){
		int i;
		for(i = 0; i < l.batch*l.inputs; ++i){
			if(state.truth[i] == SECRET_NUM) state.input[i] = SECRET_NUM;
		}
	}
	if(l.cost_type == SMOOTH){
		smooth_l1_cpu(l.batch*l.inputs, state.input, state.truth, l.delta, l.output);
	} else {
		l2_cpu(l.batch*l.inputs, state.input, state.truth, l.delta, l.output);
	}
	l.cost[0] = sum_array(l.output, l.batch*l.inputs);
}

void backward_cost_layer(Darknet::Layer & l, Darknet::NetworkState state)
{
	TAT(TATPARMS);

	axpy_cpu(l.batch*l.inputs, l.scale, l.delta, 1, state.delta, 1);
}

#ifdef DARKNET_GPU

void pull_cost_layer(Darknet::Layer & l)
{
	TAT(TATPARMS);

	cuda_pull_array(l.delta_gpu, l.delta, l.batch*l.inputs);
}

void push_cost_layer(Darknet::Layer & l)
{
	TAT(TATPARMS);

	cuda_push_array(l.delta_gpu, l.delta, l.batch*l.inputs);
}

int float_abs_compare (const void * a, const void * b)
{
	TAT(TATPARMS);

	float fa = *(const float*) a;
	if (fa < 0.0f)
	{
		fa = -fa;
	}

	float fb = *(const float*) b;
	if (fb < 0.0f)
	{
		fb = -fb;
	}

	return (fa > fb) - (fa < fb);
}

void forward_cost_layer_gpu(Darknet::Layer & l, Darknet::NetworkState state)
{
	TAT(TATPARMS);

	if (!state.truth)
	{
		return;
	}

	if (l.cost_type == MASKED)
	{
		mask_ongpu(l.batch*l.inputs, state.input, SECRET_NUM, state.truth);
	}

	if (l.cost_type == SMOOTH)
	{
		smooth_l1_gpu(l.batch*l.inputs, state.input, state.truth, l.delta_gpu, l.output_gpu);
	}
	else
	{
		l2_gpu(l.batch*l.inputs, state.input, state.truth, l.delta_gpu, l.output_gpu);
	}

	if (l.ratio)
	{
		cuda_pull_array(l.delta_gpu, l.delta, l.batch*l.inputs);

		/// @todo replace qsort() unknown priority
		qsort(l.delta, l.batch*l.inputs, sizeof(float), float_abs_compare);

		int n = (1-l.ratio) * l.batch*l.inputs;
		float thresh = l.delta[n];
		thresh = 0;
		*cfg_and_state.output << thresh << std::endl;
		supp_ongpu(l.batch*l.inputs, thresh, l.delta_gpu, 1);
	}

	cuda_pull_array(l.output_gpu, l.output, l.batch*l.inputs);
	l.cost[0] = sum_array(l.output, l.batch*l.inputs);
}

void backward_cost_layer_gpu(Darknet::Layer & l, Darknet::NetworkState state)
{
	TAT(TATPARMS);

	axpy_ongpu(l.batch*l.inputs, l.scale, l.delta_gpu, 1, state.delta, 1);
}
#endif
