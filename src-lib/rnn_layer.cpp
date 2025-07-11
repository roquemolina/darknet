#include "darknet_internal.hpp"


namespace
{
	static auto & cfg_and_state = Darknet::CfgAndState::get();
}


static void increment_layer(Darknet::Layer *l, int steps)
{
	TAT(TATPARMS);

	int num = l->outputs*l->batch*steps;
	l->output += num;
	l->delta += num;
	l->x += num;
	l->x_norm += num;

#ifdef DARKNET_GPU
	l->output_gpu += num;
	l->delta_gpu += num;
	l->x_gpu += num;
	l->x_norm_gpu += num;
#endif
}

Darknet::Layer make_rnn_layer(int batch, int inputs, int hidden, int outputs, int steps, ACTIVATION activation, int batch_normalize, int log)
{
	TAT(TATPARMS);

	*cfg_and_state.output << "RNN Layer: " << inputs << " inputs, " << outputs << " outputs" << std::endl;
	batch = batch / steps;
	Darknet::Layer l = { (Darknet::ELayerType)0 };
	l.batch = batch;
	l.type = Darknet::ELayerType::RNN;
	l.steps = steps;
	l.hidden = hidden;
	l.inputs = inputs;
	l.out_w = 1;
	l.out_h = 1;
	l.out_c = outputs;

	l.state = (float*)xcalloc(batch * hidden * (steps + 1), sizeof(float));

	l.input_layer = (Darknet::Layer*)xcalloc(1, sizeof(Darknet::Layer));
	*cfg_and_state.output << "\t\t";
	*(l.input_layer) = make_connected_layer(batch, steps, inputs, hidden, activation, batch_normalize);
	l.input_layer->batch = batch;
	if (l.workspace_size < l.input_layer->workspace_size) l.workspace_size = l.input_layer->workspace_size;

	l.self_layer = (Darknet::Layer*)xcalloc(1, sizeof(Darknet::Layer));
	*cfg_and_state.output << "\t\t";
	*(l.self_layer) = make_connected_layer(batch, steps, hidden, hidden, (log==2)?LOGGY:(log==1?LOGISTIC:activation), batch_normalize);
	l.self_layer->batch = batch;
	if (l.workspace_size < l.self_layer->workspace_size) l.workspace_size = l.self_layer->workspace_size;

	l.output_layer = (Darknet::Layer*)xcalloc(1, sizeof(Darknet::Layer));
	*cfg_and_state.output << "\t\t";
	*(l.output_layer) = make_connected_layer(batch, steps, hidden, outputs, activation, batch_normalize);
	l.output_layer->batch = batch;
	if (l.workspace_size < l.output_layer->workspace_size) l.workspace_size = l.output_layer->workspace_size;

	l.outputs = outputs;
	l.output = l.output_layer->output;
	l.delta = l.output_layer->delta;

	l.forward = forward_rnn_layer;
	l.backward = backward_rnn_layer;
	l.update = update_rnn_layer;
#ifdef DARKNET_GPU
	l.forward_gpu = forward_rnn_layer_gpu;
	l.backward_gpu = backward_rnn_layer_gpu;
	l.update_gpu = update_rnn_layer_gpu;
	l.state_gpu = cuda_make_array(l.state, batch*hidden*(steps+1));
	l.output_gpu = l.output_layer->output_gpu;
	l.delta_gpu = l.output_layer->delta_gpu;
#endif

	return l;
}

void update_rnn_layer(Darknet::Layer & l, int batch, float learning_rate, float momentum, float decay)
{
	TAT(TATPARMS);

	update_connected_layer(*(l.input_layer), batch, learning_rate, momentum, decay);
	update_connected_layer(*(l.self_layer), batch, learning_rate, momentum, decay);
	update_connected_layer(*(l.output_layer), batch, learning_rate, momentum, decay);
}

void forward_rnn_layer(Darknet::Layer & l, Darknet::NetworkState state)
{
	TAT(TATPARMS);

	Darknet::NetworkState s = {0};
	s.train = state.train;
	s.workspace = state.workspace;
	int i;
	Darknet::Layer & input_layer = *(l.input_layer);
	Darknet::Layer & self_layer = *(l.self_layer);
	Darknet::Layer & output_layer = *(l.output_layer);

	fill_cpu(l.outputs * l.batch * l.steps, 0, output_layer.delta, 1);
	fill_cpu(l.hidden * l.batch * l.steps, 0, self_layer.delta, 1);
	fill_cpu(l.hidden * l.batch * l.steps, 0, input_layer.delta, 1);
	if(state.train) fill_cpu(l.hidden * l.batch, 0, l.state, 1);

	for (i = 0; i < l.steps; ++i) {

		s.input = state.input;
		forward_connected_layer(input_layer, s);

		s.input = l.state;
		forward_connected_layer(self_layer, s);

		float *old_state = l.state;
		if(state.train) l.state += l.hidden*l.batch;
		if(l.shortcut){
			copy_cpu(l.hidden * l.batch, old_state, 1, l.state, 1);
		}else{
			fill_cpu(l.hidden * l.batch, 0, l.state, 1);
		}
		axpy_cpu(l.hidden * l.batch, 1, input_layer.output, 1, l.state, 1);
		axpy_cpu(l.hidden * l.batch, 1, self_layer.output, 1, l.state, 1);

		s.input = l.state;
		forward_connected_layer(output_layer, s);

		state.input += l.inputs*l.batch;
		increment_layer(&input_layer, 1);
		increment_layer(&self_layer, 1);
		increment_layer(&output_layer, 1);
	}
}

void backward_rnn_layer(Darknet::Layer & l, Darknet::NetworkState state)
{
	TAT(TATPARMS);

	Darknet::NetworkState s = {0};
	s.train = state.train;
	s.workspace = state.workspace;
	int i;
	Darknet::Layer & input_layer = *(l.input_layer);
	Darknet::Layer & self_layer = *(l.self_layer);
	Darknet::Layer & output_layer = *(l.output_layer);

	increment_layer(&input_layer, l.steps-1);
	increment_layer(&self_layer, l.steps-1);
	increment_layer(&output_layer, l.steps-1);

	l.state += l.hidden*l.batch*l.steps;
	for (i = l.steps-1; i >= 0; --i) {
		copy_cpu(l.hidden * l.batch, input_layer.output, 1, l.state, 1);
		axpy_cpu(l.hidden * l.batch, 1, self_layer.output, 1, l.state, 1);

		s.input = l.state;
		s.delta = self_layer.delta;
		backward_connected_layer(output_layer, s);

		l.state -= l.hidden*l.batch;
		/*
		if(i > 0){
		copy_cpu(l.hidden * l.batch, input_layer.output - l.hidden*l.batch, 1, l.state, 1);
		axpy_cpu(l.hidden * l.batch, 1, self_layer.output - l.hidden*l.batch, 1, l.state, 1);
		}else{
		fill_cpu(l.hidden * l.batch, 0, l.state, 1);
		}
		*/

		s.input = l.state;
		s.delta = self_layer.delta - l.hidden*l.batch;
		if (i == 0) s.delta = 0;
		backward_connected_layer(self_layer, s);

		copy_cpu(l.hidden*l.batch, self_layer.delta, 1, input_layer.delta, 1);
		if (i > 0 && l.shortcut) axpy_cpu(l.hidden*l.batch, 1, self_layer.delta, 1, self_layer.delta - l.hidden*l.batch, 1);
		s.input = state.input + i*l.inputs*l.batch;
		if(state.delta) s.delta = state.delta + i*l.inputs*l.batch;
		else s.delta = 0;
		backward_connected_layer(input_layer, s);

		increment_layer(&input_layer, -1);
		increment_layer(&self_layer, -1);
		increment_layer(&output_layer, -1);
	}
}

#ifdef DARKNET_GPU

void pull_rnn_layer(Darknet::Layer & l)
{
	TAT(TATPARMS);

	pull_connected_layer(*(l.input_layer));
	pull_connected_layer(*(l.self_layer));
	pull_connected_layer(*(l.output_layer));
}

void push_rnn_layer(Darknet::Layer & l)
{
	TAT(TATPARMS);

	push_connected_layer(*(l.input_layer));
	push_connected_layer(*(l.self_layer));
	push_connected_layer(*(l.output_layer));
}

void update_rnn_layer_gpu(Darknet::Layer & l, int batch, float learning_rate, float momentum, float decay, float loss_scale)
{
	TAT(TATPARMS);

	update_connected_layer_gpu(*(l.input_layer), batch, learning_rate, momentum, decay, loss_scale);
	update_connected_layer_gpu(*(l.self_layer), batch, learning_rate, momentum, decay, loss_scale);
	update_connected_layer_gpu(*(l.output_layer), batch, learning_rate, momentum, decay, loss_scale);
}

void forward_rnn_layer_gpu(Darknet::Layer & l, Darknet::NetworkState state)
{
	TAT(TATPARMS);

	Darknet::NetworkState s = {0};
	s.train = state.train;
	s.workspace = state.workspace;
	int i;
	Darknet::Layer & input_layer = *(l.input_layer);
	Darknet::Layer & self_layer = *(l.self_layer);
	Darknet::Layer & output_layer = *(l.output_layer);

	fill_ongpu(l.outputs * l.batch * l.steps, 0, output_layer.delta_gpu, 1);
	fill_ongpu(l.hidden * l.batch * l.steps, 0, self_layer.delta_gpu, 1);
	fill_ongpu(l.hidden * l.batch * l.steps, 0, input_layer.delta_gpu, 1);
	if(state.train) fill_ongpu(l.hidden * l.batch, 0, l.state_gpu, 1);

	for (i = 0; i < l.steps; ++i) {

		s.input = state.input;
		forward_connected_layer_gpu(input_layer, s);

		s.input = l.state_gpu;
		forward_connected_layer_gpu(self_layer, s);

		float *old_state = l.state_gpu;
		if(state.train) l.state_gpu += l.hidden*l.batch;
		if(l.shortcut){
			copy_ongpu(l.hidden * l.batch, old_state, 1, l.state_gpu, 1);
		}else{
			fill_ongpu(l.hidden * l.batch, 0, l.state_gpu, 1);
		}
		axpy_ongpu(l.hidden * l.batch, 1, input_layer.output_gpu, 1, l.state_gpu, 1);
		axpy_ongpu(l.hidden * l.batch, 1, self_layer.output_gpu, 1, l.state_gpu, 1);

		s.input = l.state_gpu;
		forward_connected_layer_gpu(output_layer, s);

		state.input += l.inputs*l.batch;
		increment_layer(&input_layer, 1);
		increment_layer(&self_layer, 1);
		increment_layer(&output_layer, 1);
	}
}

void backward_rnn_layer_gpu(Darknet::Layer & l, Darknet::NetworkState state)
{
	TAT(TATPARMS);

	Darknet::NetworkState s = {0};
	s.train = state.train;
	s.workspace = state.workspace;
	int i;
	Darknet::Layer & input_layer = *(l.input_layer);
	Darknet::Layer & self_layer = *(l.self_layer);
	Darknet::Layer & output_layer = *(l.output_layer);
	increment_layer(&input_layer,  l.steps - 1);
	increment_layer(&self_layer,   l.steps - 1);
	increment_layer(&output_layer, l.steps - 1);
	l.state_gpu += l.hidden*l.batch*l.steps;
	for (i = l.steps-1; i >= 0; --i) {

		s.input = l.state_gpu;
		s.delta = self_layer.delta_gpu;
		backward_connected_layer_gpu(output_layer, s);

		l.state_gpu -= l.hidden*l.batch;

		copy_ongpu(l.hidden*l.batch, self_layer.delta_gpu, 1, input_layer.delta_gpu, 1);    // the same delta for Input and Self layers

		s.input = l.state_gpu;
		s.delta = self_layer.delta_gpu - l.hidden*l.batch;
		if (i == 0) s.delta = 0;
		backward_connected_layer_gpu(self_layer, s);

		//copy_ongpu(l.hidden*l.batch, self_layer.delta_gpu, 1, input_layer.delta_gpu, 1);
		if (i > 0 && l.shortcut) axpy_ongpu(l.hidden*l.batch, 1, self_layer.delta_gpu, 1, self_layer.delta_gpu - l.hidden*l.batch, 1);
		s.input = state.input + i*l.inputs*l.batch;
		if(state.delta) s.delta = state.delta + i*l.inputs*l.batch;
		else s.delta = 0;
		backward_connected_layer_gpu(input_layer, s);

		increment_layer(&input_layer,  -1);
		increment_layer(&self_layer,   -1);
		increment_layer(&output_layer, -1);
	}
}
#endif
