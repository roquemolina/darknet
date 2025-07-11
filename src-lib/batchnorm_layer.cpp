#include "darknet_internal.hpp"


void backward_scale_cpu(float *x_norm, float *delta, int batch, int n, int size, float *scale_updates)
{
	TAT(TATPARMS);

	int i,b,f;
	for(f = 0; f < n; ++f){
		float sum = 0;
		for(b = 0; b < batch; ++b){
			for(i = 0; i < size; ++i){
				int index = i + size*(f + n*b);
				sum += delta[index] * x_norm[index];
			}
		}
		scale_updates[f] += sum;
	}
}

void mean_delta_cpu(float *delta, float *variance, int batch, int filters, int spatial, float *mean_delta)
{
	TAT(TATPARMS);

	int i,j,k;
	for(i = 0; i < filters; ++i){
		mean_delta[i] = 0;
		for (j = 0; j < batch; ++j) {
			for (k = 0; k < spatial; ++k) {
				int index = j*filters*spatial + i*spatial + k;
				mean_delta[i] += delta[index];
			}
		}
		mean_delta[i] *= (-1./sqrt(variance[i] + .00001f));
	}
}
void  variance_delta_cpu(float *x, float *delta, float *mean, float *variance, int batch, int filters, int spatial, float *variance_delta)
{
	TAT(TATPARMS);

	int i,j,k;
	for(i = 0; i < filters; ++i){
		variance_delta[i] = 0;
		for(j = 0; j < batch; ++j){
			for(k = 0; k < spatial; ++k){
				int index = j*filters*spatial + i*spatial + k;
				variance_delta[i] += delta[index]*(x[index] - mean[i]);
			}
		}
		variance_delta[i] *= -.5 * pow(variance[i] + .00001f, (float)(-3./2.));
	}
}
void normalize_delta_cpu(float *x, float *mean, float *variance, float *mean_delta, float *variance_delta, int batch, int filters, int spatial, float *delta)
{
	TAT(TATPARMS);

	int f, j, k;
	for(j = 0; j < batch; ++j){
		for(f = 0; f < filters; ++f){
			for(k = 0; k < spatial; ++k){
				int index = j*filters*spatial + f*spatial + k;
				delta[index] = delta[index] * 1./(sqrt(variance[f]) + .00001f) + variance_delta[f] * 2. * (x[index] - mean[f]) / (spatial * batch) + mean_delta[f]/(spatial*batch);
			}
		}
	}
}

void resize_batchnorm_layer(Darknet::Layer *l, int w, int h)
{
	TAT(TATPARMS);

	l->out_h = l->h = h;
	l->out_w = l->w = w;
	l->outputs = l->inputs = h*w*l->c;

	const int output_size = l->outputs * l->batch;

	l->output = (float*)realloc(l->output, output_size * sizeof(float));
	l->delta = (float*)realloc(l->delta, output_size * sizeof(float));

#ifdef DARKNET_GPU
	cuda_free(l->output_gpu);
	l->output_gpu = cuda_make_array(l->output, output_size);

	if (l->train) {
		cuda_free(l->delta_gpu);
		l->delta_gpu = cuda_make_array(l->delta, output_size);

		cuda_free(l->x_gpu);
		l->x_gpu = cuda_make_array(l->output, output_size);
#ifndef CUDNN
		cuda_free(l->x_norm_gpu);
		l->x_norm_gpu = cuda_make_array(l->output, output_size);
#endif  // not CUDNN
	}


#ifdef CUDNN
	CHECK_CUDNN(cudnnDestroyTensorDescriptor(l->normDstTensorDesc));
	CHECK_CUDNN(cudnnCreateTensorDescriptor(&l->normDstTensorDesc));
	CHECK_CUDNN(cudnnSetTensor4dDescriptor(l->normDstTensorDesc, CUDNN_TENSOR_NCHW, CUDNN_DATA_FLOAT, l->batch, l->out_c, l->out_h, l->out_w));
#endif // CUDNN
#endif // DARKNET_GPU
}

void forward_batchnorm_layer(Darknet::Layer & l, Darknet::NetworkState state)
{
	TAT(TATPARMS);

//	if(l.type == BATCHNORM) copy_cpu(l.outputs*l.batch, state.input, 1, l.output, 1);
	if(l.type == Darknet::ELayerType::CONNECTED){
		l.out_c = l.outputs;
		l.out_h = l.out_w = 1;
	}
	if(state.train){
		mean_cpu(l.output, l.batch, l.out_c, l.out_h*l.out_w, l.mean);
		variance_cpu(l.output, l.mean, l.batch, l.out_c, l.out_h*l.out_w, l.variance);

		scal_cpu(l.out_c, .9, l.rolling_mean, 1);
		axpy_cpu(l.out_c, .1, l.mean, 1, l.rolling_mean, 1);
		scal_cpu(l.out_c, .9, l.rolling_variance, 1);
		axpy_cpu(l.out_c, .1, l.variance, 1, l.rolling_variance, 1);

		copy_cpu(l.outputs*l.batch, l.output, 1, l.x, 1);
		normalize_cpu(l.output, l.mean, l.variance, l.batch, l.out_c, l.out_h*l.out_w);
		copy_cpu(l.outputs*l.batch, l.output, 1, l.x_norm, 1);
	} else {
		normalize_cpu(l.output, l.rolling_mean, l.rolling_variance, l.batch, l.out_c, l.out_h*l.out_w);
	}
	scale_bias(l.output, l.scales, l.batch, l.out_c, l.out_h*l.out_w);
	add_bias(l.output, l.biases, l.batch, l.out_c, l.out_w*l.out_h);
}

void backward_batchnorm_layer(Darknet::Layer & l, Darknet::NetworkState state)
{
	TAT(TATPARMS);

	backward_scale_cpu(l.x_norm, l.delta, l.batch, l.out_c, l.out_w*l.out_h, l.scale_updates);

	scale_bias(l.delta, l.scales, l.batch, l.out_c, l.out_h*l.out_w);

	mean_delta_cpu(l.delta, l.variance, l.batch, l.out_c, l.out_w*l.out_h, l.mean_delta);
	variance_delta_cpu(l.x, l.delta, l.mean, l.variance, l.batch, l.out_c, l.out_w*l.out_h, l.variance_delta);
	normalize_delta_cpu(l.x, l.mean, l.variance, l.mean_delta, l.variance_delta, l.batch, l.out_c, l.out_w*l.out_h, l.delta);
//	if(l.type == BATCHNORM) copy_cpu(l.outputs*l.batch, l.delta, 1, state.delta, 1);
}

void update_batchnorm_layer(Darknet::Layer & l, int batch, float learning_rate, float momentum, float decay)
{
	TAT(TATPARMS);

	//int size = l.nweights;
	axpy_cpu(l.c, learning_rate / batch, l.bias_updates, 1, l.biases, 1);
	scal_cpu(l.c, momentum, l.bias_updates, 1);

	axpy_cpu(l.c, learning_rate / batch, l.scale_updates, 1, l.scales, 1);
	scal_cpu(l.c, momentum, l.scale_updates, 1);
}

#ifdef DARKNET_GPU

void pull_batchnorm_layer(Darknet::Layer & l)
{
	TAT(TATPARMS);

	cuda_pull_array(l.biases_gpu, l.biases, l.out_c);
	cuda_pull_array(l.scales_gpu, l.scales, l.out_c);
	cuda_pull_array(l.rolling_mean_gpu, l.rolling_mean, l.out_c);
	cuda_pull_array(l.rolling_variance_gpu, l.rolling_variance, l.out_c);
}
void push_batchnorm_layer(Darknet::Layer & l)
{
	TAT(TATPARMS);

	cuda_push_array(l.biases_gpu, l.biases, l.out_c);
	cuda_push_array(l.scales_gpu, l.scales, l.out_c);
	cuda_push_array(l.rolling_mean_gpu, l.rolling_mean, l.out_c);
	cuda_push_array(l.rolling_variance_gpu, l.rolling_variance, l.out_c);
}

void forward_batchnorm_layer_gpu(Darknet::Layer & l, Darknet::NetworkState state)
{
	TAT(TATPARMS);

#if 0
	if (l.type == BATCHNORM)
	{
		simple_copy_ongpu(l.outputs*l.batch, state.input, l.output_gpu);
	}
		//copy_ongpu(l.outputs*l.batch, state.input, 1, l.output_gpu, 1);
#endif

	if (state.net.adversarial) {
		normalize_gpu(l.output_gpu, l.rolling_mean_gpu, l.rolling_variance_gpu, l.batch, l.out_c, l.out_h*l.out_w);
		scale_bias_gpu(l.output_gpu, l.scales_gpu, l.batch, l.out_c, l.out_h*l.out_w);
		add_bias_gpu(l.output_gpu, l.biases_gpu, l.batch, l.out_c, l.out_w*l.out_h);
		return;
	}

	if (state.train) {
		simple_copy_ongpu(l.outputs*l.batch, l.output_gpu, l.x_gpu);

		// cbn
		if (l.batch_normalize == 2) {

			fast_mean_gpu(l.output_gpu, l.batch, l.out_c, l.out_h*l.out_w, l.mean_gpu);

			//fast_v_gpu(l.output_gpu, l.mean_gpu, l.batch, l.out_c, l.out_h*l.out_w, l.v_cbn_gpu);
			const int minibatch_index = state.net.current_subdivision + 1;
			const int max_minibatch_index = state.net.subdivisions;
			const float alpha = 0.01;

			int inverse_variance = 0;
#ifdef CUDNN
			inverse_variance = 1;
#endif  // CUDNN

			fast_v_cbn_gpu(l.output_gpu, l.mean_gpu, l.batch, l.out_c, l.out_h*l.out_w, minibatch_index, max_minibatch_index, l.m_cbn_avg_gpu, l.v_cbn_avg_gpu, l.variance_gpu,
				alpha, l.rolling_mean_gpu, l.rolling_variance_gpu, inverse_variance, .00001);

			normalize_scale_bias_gpu(l.output_gpu, l.mean_gpu, l.variance_gpu, l.scales_gpu, l.biases_gpu, l.batch, l.out_c, l.out_h*l.out_w, inverse_variance, .00001f);

#ifndef CUDNN
			simple_copy_ongpu(l.outputs*l.batch, l.output_gpu, l.x_norm_gpu);
#endif  // CUDNN
		}
		else
		{
#ifdef CUDNN
			float one = 1;
			float zero = 0;
			cudnnBatchNormalizationForwardTraining(cudnn_handle(),
				CUDNN_BATCHNORM_SPATIAL,
				&one,
				&zero,
				l.normDstTensorDesc,
				l.x_gpu,                // input
				l.normDstTensorDesc,
				l.output_gpu,            // output
				l.normTensorDesc,
				l.scales_gpu,
				l.biases_gpu,
				.01,
				l.rolling_mean_gpu,        // output (should be FP32)
				l.rolling_variance_gpu,    // output (should be FP32)
				.00001,
				l.mean_gpu,            // output (should be FP32)
				l.variance_gpu);    // output (should be FP32)

			if (state.net.try_fix_nan) {
				fix_nan_and_inf(l.scales_gpu, l.n);
				fix_nan_and_inf(l.biases_gpu, l.n);
				fix_nan_and_inf(l.mean_gpu, l.n);
				fix_nan_and_inf(l.variance_gpu, l.n);
				fix_nan_and_inf(l.rolling_mean_gpu, l.n);
				fix_nan_and_inf(l.rolling_variance_gpu, l.n);
			}

			//simple_copy_ongpu(l.outputs*l.batch, l.output_gpu, l.x_norm_gpu);
#else   // CUDNN
			fast_mean_gpu(l.output_gpu, l.batch, l.out_c, l.out_h*l.out_w, l.mean_gpu);
			fast_variance_gpu(l.output_gpu, l.mean_gpu, l.batch, l.out_c, l.out_h*l.out_w, l.variance_gpu);

			scal_ongpu(l.out_c, .99, l.rolling_mean_gpu, 1);
			axpy_ongpu(l.out_c, .01, l.mean_gpu, 1, l.rolling_mean_gpu, 1);
			scal_ongpu(l.out_c, .99, l.rolling_variance_gpu, 1);
			axpy_ongpu(l.out_c, .01, l.variance_gpu, 1, l.rolling_variance_gpu, 1);

			copy_ongpu(l.outputs*l.batch, l.output_gpu, 1, l.x_gpu, 1);
			normalize_gpu(l.output_gpu, l.mean_gpu, l.variance_gpu, l.batch, l.out_c, l.out_h*l.out_w);
			copy_ongpu(l.outputs*l.batch, l.output_gpu, 1, l.x_norm_gpu, 1);

			scale_bias_gpu(l.output_gpu, l.scales_gpu, l.batch, l.out_c, l.out_h*l.out_w);
			add_bias_gpu(l.output_gpu, l.biases_gpu, l.batch, l.out_c, l.out_w*l.out_h);
#endif  // CUDNN
		}
	}
	else {
		normalize_gpu(l.output_gpu, l.rolling_mean_gpu, l.rolling_variance_gpu, l.batch, l.out_c, l.out_h*l.out_w);
		scale_bias_gpu(l.output_gpu, l.scales_gpu, l.batch, l.out_c, l.out_h*l.out_w);
		add_bias_gpu(l.output_gpu, l.biases_gpu, l.batch, l.out_c, l.out_w*l.out_h);
	}

}

void backward_batchnorm_layer_gpu(Darknet::Layer & l, Darknet::NetworkState state)
{
	TAT(TATPARMS);

	if (state.net.adversarial) {
		inverse_variance_ongpu(l.out_c, l.rolling_variance_gpu, l.variance_gpu, 0.00001);

		scale_bias_gpu(l.delta_gpu, l.variance_gpu, l.batch, l.out_c, l.out_h*l.out_w);
		scale_bias_gpu(l.delta_gpu, l.scales_gpu, l.batch, l.out_c, l.out_h*l.out_w);
		return;
	}

	if (!state.train) {
		//l.mean_gpu = l.rolling_mean_gpu;
		//l.variance_gpu = l.rolling_variance_gpu;
		simple_copy_ongpu(l.out_c, l.rolling_mean_gpu, l.mean_gpu);
#ifdef CUDNN
		inverse_variance_ongpu(l.out_c, l.rolling_variance_gpu, l.variance_gpu, 0.00001);
#else
		simple_copy_ongpu(l.out_c, l.rolling_variance_gpu, l.variance_gpu);
#endif
	}

#ifdef CUDNN
	float one = 1;
	float zero = 0;
	cudnnBatchNormalizationBackward(cudnn_handle(),
		CUDNN_BATCHNORM_SPATIAL,
		&one,
		&zero,
		&one,
		&one,
		l.normDstTensorDesc,
		l.x_gpu,                // input
		l.normDstTensorDesc,
		l.delta_gpu,            // input
		l.normDstTensorDesc,
		l.output_gpu, //l.x_norm_gpu,            // output
		l.normTensorDesc,
		l.scales_gpu,            // input (should be FP32)
		l.scale_updates_gpu,    // output (should be FP32)
		l.bias_updates_gpu,        // output (should be FP32)
		.00001,
		l.mean_gpu,                // input (should be FP32)
		l.variance_gpu);        // input (should be FP32)
	simple_copy_ongpu(l.outputs*l.batch, l.output_gpu, l.delta_gpu);
	//simple_copy_ongpu(l.outputs*l.batch, l.x_norm_gpu, l.delta_gpu);
#else   // CUDNN
	backward_bias_gpu(l.bias_updates_gpu, l.delta_gpu, l.batch, l.out_c, l.out_w*l.out_h);
	backward_scale_gpu(l.x_norm_gpu, l.delta_gpu, l.batch, l.out_c, l.out_w*l.out_h, l.scale_updates_gpu);

	scale_bias_gpu(l.delta_gpu, l.scales_gpu, l.batch, l.out_c, l.out_h*l.out_w);

	fast_mean_delta_gpu(l.delta_gpu, l.variance_gpu, l.batch, l.out_c, l.out_w*l.out_h, l.mean_delta_gpu);
	fast_variance_delta_gpu(l.x_gpu, l.delta_gpu, l.mean_gpu, l.variance_gpu, l.batch, l.out_c, l.out_w*l.out_h, l.variance_delta_gpu);
	normalize_delta_gpu(l.x_gpu, l.mean_gpu, l.variance_gpu, l.mean_delta_gpu, l.variance_delta_gpu, l.batch, l.out_c, l.out_w*l.out_h, l.delta_gpu);
#endif  // CUDNN

#if 0
	if (l.type == BATCHNORM)
	{
		simple_copy_ongpu(l.outputs*l.batch, l.delta_gpu, state.delta);
	}
		//copy_ongpu(l.outputs*l.batch, l.delta_gpu, 1, state.delta, 1);
#endif

	if (state.net.try_fix_nan) {
		fix_nan_and_inf(l.scale_updates_gpu, l.n);
		fix_nan_and_inf(l.bias_updates_gpu, l.n);
	}
}

void update_batchnorm_layer_gpu(Darknet::Layer & l, int batch, float learning_rate_init, float momentum, float decay, float loss_scale)
{
	TAT(TATPARMS);

	float learning_rate = learning_rate_init * l.learning_rate_scale / loss_scale;
	//float momentum = a.momentum;
	//float decay = a.decay;
	//int batch = a.batch;

	axpy_ongpu(l.c, learning_rate / batch, l.bias_updates_gpu, 1, l.biases_gpu, 1);
	scal_ongpu(l.c, momentum, l.bias_updates_gpu, 1);

	axpy_ongpu(l.c, learning_rate / batch, l.scale_updates_gpu, 1, l.scales_gpu, 1);
	scal_ongpu(l.c, momentum, l.scale_updates_gpu, 1);
}

#endif  // DARKNET_GPU
