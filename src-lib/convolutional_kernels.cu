#include "darknet_internal.hpp"
#include "gemm.hpp"
#include "col2im.hpp"
#include "im2col.hpp"


namespace
{
	static auto & cfg_and_state = Darknet::CfgAndState::get();
}


__global__ void binarize_kernel(float *x, int n, float *binary)
{
	int i = (blockIdx.x + blockIdx.y*gridDim.x) * blockDim.x + threadIdx.x;
	if (i >= n) return;
	binary[i] = (x[i] >= 0) ? 1 : -1;
}

void binarize_gpu(float *x, int n, float *binary)
{
	TAT(TATPARMS);

	binarize_kernel<<<cuda_gridsize(n), BLOCK, 0, get_cuda_stream() >>>(x, n, binary);
	CHECK_CUDA(cudaPeekAtLastError());
}

__global__ void binarize_input_kernel(float *input, int n, int size, float *binary)
{
	int s = (blockIdx.x + blockIdx.y*gridDim.x) * blockDim.x + threadIdx.x;
	if (s >= size) return;
	int i = 0;
	float mean = 0;
	for(i = 0; i < n; ++i){
		mean += fabs(input[i*size + s]);
	}
	mean = mean / n;
	for(i = 0; i < n; ++i){
		binary[i*size + s] = (input[i*size + s] > 0) ? mean : -mean;
	}
}

void binarize_input_gpu(float *input, int n, int size, float *binary)
{
	TAT(TATPARMS);

	binarize_input_kernel<<<cuda_gridsize(size), BLOCK, 0, get_cuda_stream() >>>(input, n, size, binary);
	CHECK_CUDA(cudaPeekAtLastError());
}

__global__ void binarize_weights_kernel(float *weights, int n, int size, float *binary)
{
	int f = (blockIdx.x + blockIdx.y*gridDim.x) * blockDim.x + threadIdx.x;
	if (f >= n) return;
	int i = 0;
	float mean = 0;
	for (i = 0; i < size; ++i)
	{
		mean += fabs(weights[f*size + i]);
	}
	mean = mean / size;
	for (i = 0; i < size; ++i)
	{
		binary[f*size + i] = (weights[f*size + i] > 0) ? mean : -mean;
	}
}

void binarize_weights_gpu(float *weights, int n, int size, float *binary)
{
	TAT(TATPARMS);

	binarize_weights_kernel <<<cuda_gridsize(n), BLOCK, 0, get_cuda_stream() >>>(weights, n, size, binary);
	CHECK_CUDA(cudaPeekAtLastError());
}


__global__ void set_zero_kernel(float *src, int size)
{
	int i = blockIdx.x * blockDim.x + threadIdx.x;
	if (i < size) src[i] = 0;
}

__inline__ __device__ float warpAllReduceSum(float val)
{
	for (int mask = WARP_SIZE / 2; mask > 0; mask /= 2)
#if CUDART_VERSION >= 9000
		val += __shfl_xor_sync(0xffffffff, val, mask);
#else
		val += __shfl_xor(val, mask);
#endif
	return val;
}

// only if (size % 32 == 0)
__global__ void reduce_kernel(float *weights, int n, int size, float *mean_arr_gpu)
{
	int i = blockIdx.x * blockDim.x + threadIdx.x;
	int f = i / size;
	if (f >= n) return;
	float warp_mean = warpAllReduceSum(fabs(weights[i]));
	if (i % 32 == 0)
	{
		atomicAdd(&mean_arr_gpu[f], warp_mean / size);
	}
}

__global__ void binarize_weights_mean_kernel(float *weights, int n, int size, float *binary, float *mean_arr_gpu)
{
	int i = blockIdx.x * blockDim.x + threadIdx.x;
	int f = i / size;
	if (f >= n) return;
	float mean = mean_arr_gpu[f];
	binary[i] = (weights[i] > 0) ? mean : -mean;
}

void fast_binarize_weights_gpu(float *weights, int n, int size, float *binary, float *mean_arr_gpu)
{
	TAT(TATPARMS);

	if (size % 32 == 0) {
		size_t gridsize = n * size;
		const int num_blocks = get_number_of_blocks(gridsize, BLOCK);// gridsize / BLOCK + 1;

		set_zero_kernel <<<(n/BLOCK + 1), BLOCK, 0, get_cuda_stream() >>> (mean_arr_gpu, n);
		reduce_kernel <<<num_blocks, BLOCK, 0, get_cuda_stream() >>> (weights, n, size, mean_arr_gpu);
		binarize_weights_mean_kernel <<<num_blocks, BLOCK, 0, get_cuda_stream() >>> (weights, n, size, binary, mean_arr_gpu);
		CHECK_CUDA(cudaPeekAtLastError());
	}
	else {
		binarize_weights_gpu(weights, n, size, binary);
	}
}


__global__ void cuda_f32_to_f16(float* input_f32, size_t size, half *output_f16)
{
	int idx = blockIdx.x * blockDim.x + threadIdx.x;
	if (idx < size) output_f16[idx] = __float2half(input_f32[idx]);
}

void cuda_convert_f32_to_f16(float* input_f32, size_t size, float *output_f16)
{
	TAT(TATPARMS);

	cuda_f32_to_f16 <<< get_number_of_blocks(size, BLOCK), BLOCK, 0, get_cuda_stream() >>> (input_f32, size, (half *)output_f16);
	CHECK_CUDA(cudaPeekAtLastError());
}

__global__ void cuda_f16_to_f32(half* input_f16, size_t size, float *output_f32)
{
	int idx = blockIdx.x * blockDim.x + threadIdx.x;
	if (idx < size) output_f32[idx] = __half2float(input_f16[idx]);
}

void cuda_convert_f16_to_f32(float* input_f16, size_t size, float *output_f32)
{
	TAT(TATPARMS);

	cuda_f16_to_f32 <<< get_number_of_blocks(size, BLOCK), BLOCK, 0, get_cuda_stream() >>> ((half *)input_f16, size, output_f32);
	CHECK_CUDA(cudaPeekAtLastError());
}

half *cuda_make_f16_from_f32_array(float *src, size_t n)
{
	TAT(TATPARMS);

	half *dst16;
	size_t size = sizeof(half)*n;
	CHECK_CUDA(cudaMalloc((void **)&dst16, size));
	if (src) {
		assert(n > 0);
		cuda_convert_f32_to_f16(src, n, (float *)dst16);
	}
	if (!dst16)
	{
		darknet_fatal_error(DARKNET_LOC, "CUDA malloc failed (n=%d)", n);
	}
	return dst16;
}

void forward_convolutional_layer_gpu(Darknet::Layer & l, Darknet::NetworkState state)
{
	TAT(TATPARMS);

	if (l.train == 0) state.train = 0;

	if (l.stream >= 0) {
		switch_stream(l.stream);
	}

	if (l.wait_stream_id >= 0) {
		wait_stream(l.wait_stream_id);
	}

	//fill_ongpu(l.outputs*l.batch, 0, l.output_gpu, 1);
	if (l.binary)
	{
		binarize_weights_gpu(l.weights_gpu, l.n, (l.c / l.groups)*l.size*l.size, l.binary_weights_gpu);
		swap_binary(&l);
	}

	if (l.xnor)
	{
		if (!l.align_bit_weights_gpu || state.train)
		{
			fast_binarize_weights_gpu(l.weights_gpu, l.n, (l.c / l.groups)*l.size*l.size, l.binary_weights_gpu, l.mean_arr_gpu);
		}

		if (l.align_bit_weights_gpu && !state.train && l.c >= 32 && l.stride_x == l.stride_y)
		{
			int m = l.n / l.groups;
			int k = l.size*l.size*l.c / l.groups;
			int n = l.out_w*l.out_h;

			const int ldb_align = l.lda_align;
			const size_t new_ldb = k + (ldb_align - k%ldb_align); // (k / 8 + 1) * 8;

			if (l.c % 32 == 0)
			{
				const int new_c = l.c / 32;

				repack_input_gpu_bin(state.input, (uint32_t *)l.align_workspace_gpu, l.w, l.h, l.c);

				im2col_ongpu(l.align_workspace_gpu, new_c, l.h, l.w, l.size, l.stride, l.pad, state.workspace);

				int new_k = l.size*l.size*l.c / 32;

				transpose_uint32_gpu((uint32_t *)state.workspace, (uint32_t *)l.transposed_align_workspace_gpu, new_k, n, n, new_ldb);
				gemm_nn_custom_bin_mean_transposed_gpu(m, n, k,
					(unsigned char *)l.align_bit_weights_gpu, new_ldb, (unsigned char *)l.transposed_align_workspace_gpu,
					new_ldb, l.output_gpu, n, l.mean_arr_gpu, l.biases_gpu, l.activation == LEAKY,
					l.bin_conv_shortcut_in_gpu, l.bin_conv_shortcut_out_gpu);
			}
			else
			{
				int i = 0;
				{
					im2col_align_ongpu(state.input + i*l.c*l.h*l.w, l.c, l.h, l.w, l.size, l.stride, l.pad, l.align_workspace_gpu, l.bit_align);

					// should be optimized
					float_to_bit_gpu(l.align_workspace_gpu, (unsigned char *)state.workspace, l.align_workspace_size);
				}
				transpose_bin_gpu((unsigned char *)state.workspace, (unsigned char *)l.transposed_align_workspace_gpu, k, n, l.bit_align, new_ldb, 8);

				gemm_nn_custom_bin_mean_transposed_gpu(m, n, k,
						(unsigned char *)l.align_bit_weights_gpu, new_ldb, (unsigned char *)l.transposed_align_workspace_gpu,
						new_ldb, l.output_gpu, n, l.mean_arr_gpu, l.biases_gpu, l.activation == LEAKY,
						l.bin_conv_shortcut_in_gpu, l.bin_conv_shortcut_out_gpu);
			}

			if (l.activation == SWISH) activate_array_swish_ongpu(l.output_gpu, l.outputs*l.batch, l.activation_input_gpu, l.output_gpu);
			else if (l.activation == MISH) activate_array_mish_ongpu(l.output_gpu, l.outputs*l.batch, l.activation_input_gpu, l.output_gpu);
			else if (l.activation == HARD_MISH) activate_array_hard_mish_ongpu(l.output_gpu, l.outputs*l.batch, l.activation_input_gpu, l.output_gpu);
			else if (l.activation == NORM_CHAN) activate_array_normalize_channels_ongpu(l.output_gpu, l.outputs*l.batch, l.batch, l.out_c, l.out_w*l.out_h, l.output_gpu);
			else if (l.activation == NORM_CHAN_SOFTMAX) activate_array_normalize_channels_softmax_ongpu(l.output_gpu, l.outputs*l.batch, l.batch, l.out_c, l.out_w*l.out_h, l.output_gpu, 0);
			else if (l.activation == NORM_CHAN_SOFTMAX_MAXVAL) activate_array_normalize_channels_softmax_ongpu(l.output_gpu, l.outputs*l.batch, l.batch, l.out_c, l.out_w*l.out_h, l.output_gpu, 1);
			else if (l.activation != LINEAR && l.activation != LEAKY) activate_array_ongpu(l.output_gpu, l.outputs*l.batch, l.activation);
			return;
		}
	}

	if (l.xnor)
	{
		swap_binary(&l);
		binarize_gpu(state.input, l.c*l.h*l.w*l.batch, l.binary_input_gpu);
		state.input = l.binary_input_gpu;
	}

	//fill_ongpu(l.outputs*l.batch, 0, l.output_gpu, 1);

#ifdef CUDNN
	//float one = 1;    // alpha[0], beta[0] is float for HALF and FLOAT
	float alpha = 1, beta = 0;

//#ifdef CUDNN_HALF
	//if (state.use_mixed_precision) {
	int iteration_num = get_current_iteration(state.net); // (*state.net.seen) / (state.net.batch*state.net.subdivisions);
	if (state.index != 0 && state.net.cudnn_half && !l.xnor && (!state.train || (iteration_num > 3 * state.net.burn_in) && state.net.loss_scale != 1) &&
		(l.c / l.groups) % 8 == 0 && l.n % 8 == 0 && l.groups <= 1 && l.size > 1)
	{
		// Note: For improved performance it is advised to use beta[0] = 0.0.
		// For Tensor Core: cudnnSetConvolutionMathType() where cudnnMathType_t mathType = CUDNN_TENSOR_OP_MATH;
		// 1. or CUDNN_CONVOLUTION_FWD_ALGO_IMPLICIT_PRECOMP_GEMM and use CUDNN_DATA_HALF
		// 2. or CUDNN_CONVOLUTION_FWD_ALGO_WINOGRAD_NONFUSED
		// More: http://docs.nvidia.com/deeplearning/sdk/cudnn-developer-guide/index.html#tensor_ops

		const size_t input16_size = l.batch*l.c*l.w*l.h;
		const size_t output16_size = l.batch*l.out_c*l.out_h*l.out_w;

		if (*state.net.max_input16_size < input16_size)
		{
			*state.net.max_input16_size = input16_size;
			if (*state.net.input16_gpu) cuda_free(*state.net.input16_gpu);
			assert(*state.net.max_input16_size > 0);
			*state.net.input16_gpu = (float *)cuda_make_f16_from_f32_array(NULL, *state.net.max_input16_size);
		}
		float *input16 = *state.net.input16_gpu;

		if (*state.net.max_output16_size < output16_size) {
			*state.net.max_output16_size = output16_size;
			if (*state.net.output16_gpu) cuda_free(*state.net.output16_gpu);
			assert(*state.net.max_output16_size > 0);
			*state.net.output16_gpu = (float *)cuda_make_f16_from_f32_array(NULL, *state.net.max_output16_size);
		}
		float *output16 = *state.net.output16_gpu;

		assert(input16_size > 0);
		cuda_convert_f32_to_f16(state.input, input16_size, input16);

		CHECK_CUDNN(cudnnConvolutionForward(cudnn_handle(),
			&alpha,
			l.srcTensorDesc16,
			input16,
			l.weightDesc16,
			l.weights_gpu16,
			l.convDesc,
			l.fw_algo16,
			state.workspace,
			l.workspace_size,
			&beta,
			l.dstTensorDesc16,
			output16));


		if (l.batch_normalize)
		{
			if (state.train && !state.net.adversarial) // Training
			{
				simple_copy_ongpu(l.outputs*l.batch / 2, output16, l.x_gpu);
				float one = 1.0f;
				float zero = 0.0f;
				// Batch-normalization can still take FP16 inputs and outputs, saving half the bandwidth
				// compared to FP32, it's just that the statistics and value adjustment should be done in FP32.
				CHECK_CUDNN(cudnnBatchNormalizationForwardTraining(cudnn_handle(),
					CUDNN_BATCHNORM_SPATIAL,
					&one,
					&zero,
					l.normDstTensorDescF16,
					l.x_gpu,            // input
					l.normDstTensorDescF16,
					output16,            // output
					l.normTensorDesc,
					l.scales_gpu,       // input
					l.biases_gpu,       // input
					.01,
					l.rolling_mean_gpu,        // input/output (should be FP32)
					l.rolling_variance_gpu,    // input/output (should be FP32)
					.00001,
					l.mean_gpu,            // output (should be FP32) - optional cache to speedup cudnnBatchNormalizationBackward()
					l.variance_gpu));    // output (should be FP32) - optional cache to speedup cudnnBatchNormalizationBackward()

				cuda_convert_f16_to_f32(output16, output16_size, l.output_gpu);
				//forward_batchnorm_layer_gpu(l, state);
			}
			else // Detection
			{
				cuda_convert_f16_to_f32(output16, output16_size, l.output_gpu);
				normalize_gpu(l.output_gpu, l.rolling_mean_gpu, l.rolling_variance_gpu, l.batch, l.out_c, l.out_h*l.out_w);
				scale_bias_gpu(l.output_gpu, l.scales_gpu, l.batch, l.out_c, l.out_h*l.out_w);
				add_bias_gpu(l.output_gpu, l.biases_gpu, l.batch, l.out_c, l.out_w*l.out_h);
			}
		}
		else // BIAS only
		{
			cuda_convert_f16_to_f32(output16, output16_size, l.output_gpu);
			add_bias_gpu(l.output_gpu, l.biases_gpu, l.batch, l.n, l.out_w*l.out_h);
		}
	}
	else
	{
		CHECK_CUDNN(cudnnConvolutionForward(cudnn_handle(),
			&alpha, //&one,
			l.srcTensorDesc,
			state.input,
			l.weightDesc,
			l.weights_gpu,
			l.convDesc,
			l.fw_algo,
			state.workspace,
			l.workspace_size,
			&beta,  //&one,
			l.dstTensorDesc,
			l.output_gpu));

		//cudaDeviceSynchronize();
		if (l.batch_normalize) {
			forward_batchnorm_layer_gpu(l, state);
		}
		else {
			add_bias_gpu(l.output_gpu, l.biases_gpu, l.batch, l.n, l.out_w*l.out_h);
		}
	//#endif    // CUDNN_HALF
	}


#else
	fill_ongpu(l.outputs*l.batch, 0, l.output_gpu, 1);

	int i, j;
	int m = l.n / l.groups;
	int k = l.size*l.size*l.c / l.groups;
	int n = l.out_w*l.out_h;
	for(i = 0; i < l.batch; ++i){
		for (j = 0; j < l.groups; ++j) {
			//float *im = state.input + i*l.c*l.h*l.w;
			float *im = state.input + (i*l.groups + j)*l.c / l.groups*l.h*l.w;
			float *a = l.weights_gpu + j*l.nweights / l.groups;
			float *b = state.workspace;
			float *c = l.output_gpu + (i*l.groups + j)*n*m;
			if (l.size == 1 && l.stride == 1 && l.dilation == 1) {
				b = im;
			}
			else {
				//im2col_ongpu(im, l.c / l.groups, l.h, l.w, l.size, l.stride, l.pad, state.workspace);

				im2col_gpu_ext(im,          // input
					l.c / l.groups,         // input channels
					l.h, l.w,               // input size (h, w)
					l.size, l.size,         // kernel size (h, w)
					l.pad * l.dilation, l.pad * l.dilation,   // padding (h, w)
					l.stride_y, l.stride_x,     // stride (h, w)
					l.dilation, l.dilation, // dilation (h, w)
					state.workspace);       // output

			}
			//gemm_ongpu(0, 0, m, n, k, 1., a, k, b, n, 1., c + i*m*n, n);
			gemm_ongpu(0, 0, m, n, k, 1, a, k, b, n, 1, c, n);
		}
	}

	if (l.batch_normalize) {
		forward_batchnorm_layer_gpu(l, state);
	}
	else {
		add_bias_gpu(l.output_gpu, l.biases_gpu, l.batch, l.n, l.out_w*l.out_h);
	}
#endif

//#ifndef CUDNN_HALF
//#endif // no CUDNN_HALF

	if (l.activation == SWISH) activate_array_swish_ongpu(l.output_gpu, l.outputs*l.batch, l.activation_input_gpu, l.output_gpu);
	else if (l.activation == MISH) activate_array_mish_ongpu(l.output_gpu, l.outputs*l.batch, l.activation_input_gpu, l.output_gpu);
	else if (l.activation == HARD_MISH) activate_array_hard_mish_ongpu(l.output_gpu, l.outputs*l.batch, l.activation_input_gpu, l.output_gpu);
	else if (l.activation == NORM_CHAN) activate_array_normalize_channels_ongpu(l.output_gpu, l.outputs*l.batch, l.batch, l.out_c, l.out_w*l.out_h, l.output_gpu);
	else if (l.activation == NORM_CHAN_SOFTMAX) activate_array_normalize_channels_softmax_ongpu(l.output_gpu, l.outputs*l.batch, l.batch, l.out_c, l.out_w*l.out_h, l.output_gpu, 0);
	else if (l.activation == NORM_CHAN_SOFTMAX_MAXVAL) activate_array_normalize_channels_softmax_ongpu(l.output_gpu, l.outputs*l.batch, l.batch, l.out_c, l.out_w*l.out_h, l.output_gpu, 1);
	else if (l.activation != LINEAR) activate_array_ongpu(l.output_gpu, l.outputs*l.batch, l.activation);
	//if(l.dot > 0) dot_error_gpu(l);
	if(l.binary || l.xnor) swap_binary(&l);
	//cudaDeviceSynchronize();    // for correct profiling of performance

	if (state.net.try_fix_nan) {
		fix_nan_and_inf(l.output_gpu, l.outputs*l.batch);
	}

	if(l.assisted_excitation && state.train) assisted_excitation_forward_gpu(l, state);

	if (l.antialiasing) {
		Darknet::NetworkState s = { 0 };
		s.train = state.train;
		s.workspace = state.workspace;
		s.net = state.net;
		if (!state.train) s.index = state.index;  // don't use TC for training (especially without cuda_convert_f32_to_f16() )
		s.input = l.output_gpu;
		forward_convolutional_layer_gpu(*(l.input_layer), s);
		simple_copy_ongpu(l.outputs*l.batch, l.output_gpu, l.input_antialiasing_gpu);
		simple_copy_ongpu(l.input_layer->outputs*l.input_layer->batch, l.input_layer->output_gpu, l.output_gpu);
	}

	if (l.coordconv) {
		coord_conv_gpu(l.output_gpu, l.outputs*l.batch, l.out_w, l.out_h, l.out_c, l.batch, 0);
	}
}

void backward_convolutional_layer_gpu(Darknet::Layer & l, Darknet::NetworkState state)
{
	TAT(TATPARMS);

	if (l.coordconv) {
		coord_conv_gpu(l.delta_gpu, l.outputs*l.batch, l.out_w, l.out_h, l.out_c, l.batch, 1);
	}

	if (l.antialiasing) {
		Darknet::NetworkState s = { 0 };
		s.train = state.train;
		s.workspace = state.workspace;
		s.net = state.net;
		s.delta = l.delta_gpu;  // s.delta will be returned to l.delta_gpu
		s.input = l.input_antialiasing_gpu;
		//if (!state.train) s.index = state.index;  // don't use TC for training (especially without cuda_convert_f32_to_f16() )
		simple_copy_ongpu(l.input_layer->outputs*l.input_layer->batch, l.delta_gpu, l.input_layer->delta_gpu);
		backward_convolutional_layer_gpu(*(l.input_layer), s);

		simple_copy_ongpu(l.outputs*l.batch, l.input_antialiasing_gpu, l.output_gpu);
	}

	if(state.net.try_fix_nan) constrain_ongpu(l.outputs*l.batch, 1, l.delta_gpu, 1);

	if (l.activation == SWISH) gradient_array_swish_ongpu(l.output_gpu, l.outputs*l.batch, l.activation_input_gpu, l.delta_gpu);
	else if (l.activation == MISH) gradient_array_mish_ongpu(l.outputs*l.batch, l.activation_input_gpu, l.delta_gpu);
	else if (l.activation == HARD_MISH) gradient_array_hard_mish_ongpu(l.outputs*l.batch, l.activation_input_gpu, l.delta_gpu);
	else if (l.activation == NORM_CHAN_SOFTMAX || l.activation == NORM_CHAN_SOFTMAX_MAXVAL) gradient_array_normalize_channels_softmax_ongpu(l.output_gpu, l.outputs*l.batch, l.batch, l.out_c, l.out_w*l.out_h, l.delta_gpu);
	else if (l.activation == NORM_CHAN) gradient_array_normalize_channels_ongpu(l.output_gpu, l.outputs*l.batch, l.batch, l.out_c, l.out_w*l.out_h, l.delta_gpu);
	else gradient_array_ongpu(l.output_gpu, l.outputs*l.batch, l.activation, l.delta_gpu);

	if (!l.batch_normalize)
		backward_bias_gpu(l.bias_updates_gpu, l.delta_gpu, l.batch, l.n, l.out_w*l.out_h);

//#ifndef CUDNN_HALF
	//if(l.batch_normalize){
	//    backward_batchnorm_layer_gpu(l, state);
	//} else {
	//    //backward_bias_gpu(l.bias_updates_gpu, l.delta_gpu, l.batch, l.n, l.out_w*l.out_h);
	//}
//#endif // no CUDNN_HALF
	float *original_input = state.input;

	if(l.xnor) state.input = l.binary_input_gpu;
#ifdef CUDNN
	float alpha = 1.0f;
	float beta = 0.0f;

//#ifdef CUDNN_HALF
	int iteration_num = get_current_iteration(state.net); //(*state.net.seen) / (state.net.batch*state.net.subdivisions);
	if (state.index != 0 && state.net.cudnn_half && !l.xnor && (!state.train || (iteration_num > 3 * state.net.burn_in) && state.net.loss_scale != 1) &&
		(l.c / l.groups) % 8 == 0 && l.n % 8 == 0  && l.groups <= 1 && l.size > 1)
	{
		const size_t input16_size = l.batch*l.c*l.w*l.h;
		const size_t delta16_size = l.batch*l.n*l.out_w*l.out_h;

		if (*state.net.max_input16_size < input16_size) {
			*state.net.max_input16_size = input16_size;
			if (*state.net.input16_gpu) cuda_free(*state.net.input16_gpu);
			assert(*state.net.max_input16_size > 0);
			*state.net.input16_gpu = (float *)cuda_make_f16_from_f32_array(NULL, *state.net.max_input16_size);
		}
		float *input16 = *state.net.input16_gpu;

		if (*state.net.max_output16_size < delta16_size) {
			*state.net.max_output16_size = delta16_size;
			if (*state.net.output16_gpu) cuda_free(*state.net.output16_gpu);
			assert(*state.net.max_output16_size > 0);
			*state.net.output16_gpu = (float *)cuda_make_f16_from_f32_array(NULL, *state.net.max_output16_size);
		}
		float *delta16 = *state.net.output16_gpu;

		assert(input16_size > 0);
		assert(delta16_size > 0);
		cuda_convert_f32_to_f16(state.input, input16_size, input16);
		cuda_convert_f32_to_f16(l.delta_gpu, delta16_size, delta16);

		if (l.batch_normalize)
		{
			float one = 1.0f;
			float zero = 0.0f;
			CHECK_CUDNN(cudnnBatchNormalizationBackward(cudnn_handle(),
				CUDNN_BATCHNORM_SPATIAL,
				&one,
				&zero,
				&one,
				&one,
				l.normDstTensorDescF16,
				l.x_gpu,                // input (input in BN-forward-inference)
				l.normDstTensorDescF16,
				delta16,                // input
				l.normDstTensorDescF16,
				l.output_gpu, //l.x_norm_gpu,            // output (new delta)
				l.normTensorDesc,
				l.scales_gpu,            // input (should be FP32)
				l.scale_updates_gpu,    // output (should be FP32)
				l.bias_updates_gpu,        // output (should be FP32)
				.00001,
				l.mean_gpu,                // input (should be FP32)
				l.variance_gpu));        // input (should be FP32)

			simple_copy_ongpu(l.outputs*l.batch / 2, l.output_gpu, delta16);
		}

		// convert input: state.input (x), l.delta_gpu (y) from fp32 to fp16
		// get output: l.weight_updates_gpu (dw) and convert it to fp32 (ONLY if it is fp16)

		// calculate conv weight updates
		// Already: l.weight_updates_gpu = (l.weight_updates_gpu - l.weight*decay*batch*subdivision)*momentum
		//   so we should copy f32 to f16, or compute: f16=(w_up - w*d*b*s)*m
		assert((l.nweights) > 0);
		cuda_convert_f32_to_f16(l.weight_updates_gpu, l.nweights, l.weight_updates_gpu16);

		float one = 1.0f;
		if (!state.net.adversarial && !l.train_only_bn) {
			CHECK_CUDNN(cudnnConvolutionBackwardFilter(cudnn_handle(),
				&one,
				l.srcTensorDesc16,
				input16, //state.input,
				l.ddstTensorDesc16,
				delta16, //l.delta_gpu,
				l.convDesc,
				l.bf_algo16,
				state.workspace,
				l.workspace_size,
				&one,
				l.dweightDesc16,
				l.weight_updates_gpu16));    // l.weight_updates_gpu);

			cuda_convert_f16_to_f32(l.weight_updates_gpu16, l.nweights, l.weight_updates_gpu);
		}

		if (state.delta) {
			if (l.binary || l.xnor) swap_binary(&l);

			// http://docs.nvidia.com/deeplearning/sdk/cudnn-developer-guide/index.html#cudnnConvolutionBackwardData
			// calculate delta for the next layer
			// convert input: l.weights_gpu (w), l.delta_gpu (dy) from fp32 to fp16
			// get output: state.delta (dx) and convert it to fp32 (ONLY if it is fp16)
			CHECK_CUDNN(cudnnConvolutionBackwardData(cudnn_handle(),
				&alpha,
				l.weightDesc16,
				l.weights_gpu16, //l.weights_gpu,
				l.ddstTensorDesc16,
				delta16, //l.delta_gpu,
				l.convDesc,
				l.bd_algo16,
				state.workspace,
				l.workspace_size,
				&beta,
				l.dsrcTensorDesc16,
				input16));    // state.delta);

			cuda_convert_f16_to_f32(input16, input16_size, state.delta);

			if (l.binary || l.xnor) swap_binary(&l);
			if (l.xnor) gradient_array_ongpu(original_input, l.batch*l.c*l.h*l.w, HARDTAN, state.delta);
		}
	}
	else {
		//#else    // CUDNN_HALF

		if(l.batch_normalize){
			backward_batchnorm_layer_gpu(l, state);
		}

		if (!state.net.adversarial && !l.train_only_bn) {

			float *old_input = state.input;

			// calculate conv weight updates
			// if used: beta=1 then loss decreases faster
			float one = 1.0f;
			CHECK_CUDNN(cudnnConvolutionBackwardFilter(cudnn_handle(),
				&one,
				l.srcTensorDesc,
				state.input,
				l.ddstTensorDesc,
				l.delta_gpu,
				l.convDesc,
				l.bf_algo,
				state.workspace,
				l.workspace_size,
				&one,
				l.dweightDesc,
				l.weight_updates_gpu));

			state.input = old_input;
		}

		if (state.delta) {
			if (l.binary || l.xnor) swap_binary(&l);

			float *old_weights = l.weights_gpu;

			// http://docs.nvidia.com/deeplearning/sdk/cudnn-developer-guide/index.html#cudnnConvolutionBackwardData
			// calculate delta for the next layer
			float one = 1.0f;
			CHECK_CUDNN(cudnnConvolutionBackwardData(cudnn_handle(),
				&one,
				l.weightDesc,
				l.weights_gpu,
				l.ddstTensorDesc,
				l.delta_gpu,
				l.convDesc,
				l.bd_algo,
				state.workspace,
				l.workspace_size,
				&one,
				l.dsrcTensorDesc,
				state.delta));

			l.weights_gpu = old_weights;

			if (l.binary || l.xnor) swap_binary(&l);
			if (l.xnor) gradient_array_ongpu(original_input, l.batch*l.c*l.h*l.w, HARDTAN, state.delta);
		}
	}

//#endif    // CUDNN_HALF

#else    // CUDNN
	if (l.batch_normalize) {
		backward_batchnorm_layer_gpu(l, state);
	}

	int m = l.n / l.groups;
	int n = l.size*l.size*l.c / l.groups;
	int k = l.out_w*l.out_h;

	int i, j;
	for(i = 0; i < l.batch; ++i){
		for (j = 0; j < l.groups; ++j) {
			float * a = l.delta_gpu + (i*l.groups + j)*m*k;
			float * b = state.workspace;
			float * c = l.weight_updates_gpu + j*l.nweights / l.groups;

			float *im = state.input + (i*l.groups + j)*l.c / l.groups*l.h*l.w;

			if (!state.net.adversarial && !l.train_only_bn) {
				//im2col_ongpu(im, l.c / l.groups, l.h, l.w, l.size, l.stride, l.pad, state.workspace);
				im2col_gpu_ext(im,          // input
					l.c / l.groups,         // input channels
					l.h, l.w,               // input size (h, w)
					l.size, l.size,         // kernel size (h, w)
					l.pad * l.dilation, l.pad * l.dilation,   // padding (h, w)
					l.stride_y, l.stride_x,     // stride (h, w)
					l.dilation, l.dilation, // dilation (h, w)
					state.workspace);       // output
				//gemm_ongpu(0, 1, m, n, k, 1, a + i*m*k, k, b, k, 1, c, n);
				gemm_ongpu(0, 1, m, n, k, 1, a, k, b, k, 1, c, n);
			}

			if (state.delta) {
				if (l.binary || l.xnor) swap_binary(&l);
				float * a = l.weights_gpu + j*l.nweights / l.groups;
				float * b = l.delta_gpu + (i*l.groups + j)*m*k;
				float * c = state.workspace;

				//gemm_ongpu(1, 0, n, k, m, 1, a, n, b + i*k*m, k, 0, c, k);
				gemm_ongpu(1, 0, n, k, m, 1, a, n, b, k, 0, c, k);


				float *delta = state.delta + (i*l.groups + j)*l.c / l.groups*l.h*l.w;

				//col2im_ongpu(state.workspace, l.c / l.groups, l.h, l.w, l.size, l.stride, l.pad, delta);
				col2im_gpu_ext(
					state.workspace,        // input
					l.c / l.groups,         // input channels
					l.h, l.w,               // input size (h, w)
					l.size, l.size,         // kernel size (h, w)
					l.pad * l.dilation, l.pad * l.dilation,   // padding size (h, w)
					l.stride_y, l.stride_x,     // stride size (h, w)
					l.dilation, l.dilation, // dilation size (h, w)
					delta);                 // output (delta)

				if (l.binary || l.xnor) {
					swap_binary(&l);
				}
				if (l.xnor) gradient_array_ongpu(original_input + i*l.c*l.h*l.w, l.c*l.h*l.w, HARDTAN, state.delta + i*l.c*l.h*l.w);
			}
		}
	}
#endif
	if (state.net.try_fix_nan) {
		if (state.delta) {
			reset_nan_and_inf(state.delta, l.inputs * l.batch);
		}
		int size = l.nweights;
		reset_nan_and_inf(l.weight_updates_gpu, size);
		fix_nan_and_inf(l.weights_gpu, size);
	}


}

__global__ void calc_avg_activation_kernel(float *src, float *dst, int size, int channels, int batches)
{
	int i = blockIdx.x * blockDim.x + threadIdx.x;
	int xy = i % size;
	int b = i / size;

	if (i < size*batches) {
		dst[i] = 0;
		for (int c = 0; c < channels; ++c) {
			dst[i] += src[xy + size*(c + channels*b)];
		}
		dst[i] = dst[i] / channels;
	}
}

void calc_avg_activation_gpu(float *src, float *dst, int size, int channels, int batches)
{
	TAT(TATPARMS);

	const int num_blocks = get_number_of_blocks(size*batches, BLOCK);

	calc_avg_activation_kernel <<<num_blocks, BLOCK, 0, get_cuda_stream() >>> (src, dst, size, channels, batches);
}


__global__ void assisted_activation_kernel(float alpha, float *output, float *gt_gpu, float *a_avg_gpu, int size, int channels, int batches)
{
	int i = blockIdx.x * blockDim.x + threadIdx.x;
	int xy = i % size;
	int b = i / size;

	if (b < batches)
	{
		for (int c = 0; c < channels; ++c)
		{
			output[xy + size*(c + channels*b)] += alpha * gt_gpu[i] * a_avg_gpu[i];
		}
	}
}

void assisted_activation_gpu(float alpha, float *output, float *gt_gpu, float *a_avg_gpu, int size, int channels, int batches)
{
	TAT(TATPARMS);

	const int num_blocks = get_number_of_blocks(size*batches, BLOCK);

	assisted_activation_kernel <<<num_blocks, BLOCK, 0, get_cuda_stream() >>> (alpha, output, gt_gpu, a_avg_gpu, size, channels, batches);
}


__global__ void assisted_activation2_kernel(float alpha, float *output, float *gt_gpu, float *a_avg_gpu, int size, int channels, int batches)
{
	int i = blockIdx.x * blockDim.x + threadIdx.x;
	int xy = i % size;
	int b = i / size;
	float beta = 1 - alpha;

	if (b < batches) {
		for (int c = 0; c < channels; ++c) {
			if(gt_gpu[i] == 0)
				output[xy + size*(c + channels*b)] *= beta;

		}
	}
}

void assisted_activation2_gpu(float alpha, float *output, float *gt_gpu, float *a_avg_gpu, int size, int channels, int batches)
{
	TAT(TATPARMS);

	const int num_blocks = get_number_of_blocks(size*batches, BLOCK);

	assisted_activation2_kernel <<<num_blocks, BLOCK, 0, get_cuda_stream() >>> (alpha, output, gt_gpu, a_avg_gpu, size, channels, batches);
}

void assisted_excitation_forward_gpu(Darknet::Layer & l, Darknet::NetworkState state)
{
	TAT(TATPARMS);

	const int iteration_num = get_current_iteration(state.net); //(*state.net.seen) / (state.net.batch*state.net.subdivisions);

	float alpha = (1 + cos(3.141592 * iteration_num / state.net.max_batches)) / 2;

	if (l.assisted_excitation == 1)
	{
		if (iteration_num > state.net.max_batches / 2) return;
	}
	else
	{
		if (iteration_num < state.net.burn_in) return;
		else
			if (iteration_num > l.assisted_excitation) return;
		else
			alpha = (1 + cos(3.141592 * iteration_num / (state.net.burn_in + l.assisted_excitation))) / 2; // from 1 to 0
	}

	float *a_avg = (float *)calloc(l.out_w * l.out_h * l.batch, sizeof(float));
	float *gt = (float *)calloc(l.out_w * l.out_h * l.batch, sizeof(float));

	int b;
	int w, h;

	l.max_boxes = state.net.num_boxes;
	l.truths = l.max_boxes*(4 + 1);

	int num_truth = l.batch*l.truths;
	float *truth_cpu = (float *)calloc(num_truth, sizeof(float));
	cuda_pull_array(state.truth, truth_cpu, num_truth);

	for (b = 0; b < l.batch; ++b)
	{
		// calculate G
		int t;
		for (t = 0; t < state.net.num_boxes; ++t) {
			Darknet::Box truth = float_to_box_stride(truth_cpu + t*(4 + 1) + b*l.truths, 1);
			if (!truth.x) break;  // continue;
			float beta = 0;
			//float beta = 1 - alpha; // from 0 to 1
			float dw = (1 - truth.w) * beta;
			float dh = (1 - truth.h) * beta;

			int left = floorf((truth.x - (dw + truth.w) / 2) * l.out_w);
			int right = ceilf((truth.x + (dw + truth.w) / 2) * l.out_w);
			int top = floorf((truth.y - (dh + truth.h) / 2) * l.out_h);
			int bottom = ceilf((truth.y + (dh + truth.h) / 2) * l.out_h);
			if (left < 0) left = 0;
			if (top < 0) top = 0;
			if (right > l.out_w) right = l.out_w;
			if (bottom > l.out_h) bottom = l.out_h;

			for (w = left; w <= right; w++) {
				for (h = top; h < bottom; h++) {
					gt[w + l.out_w * h + l.out_w*l.out_h*b] = 1;
				}
			}
		}
	}

	cuda_push_array(l.gt_gpu, gt, l.out_w * l.out_h * l.batch);

	// calc avg_output on GPU - for whole batch
	calc_avg_activation_gpu(l.output_gpu, l.a_avg_gpu, l.out_w * l.out_h, l.out_c, l.batch);

	// calc new output
	assisted_activation_gpu(alpha, l.output_gpu, l.gt_gpu, l.a_avg_gpu, l.out_w * l.out_h, l.out_c, l.batch);

	if (0)   // visualize ground truth
	{
		cuda_pull_array(l.output_gpu, l.output, l.outputs * l.batch);
		CHECK_CUDA(cudaStreamSynchronize(get_cuda_stream()));

		for (b = 0; b < l.batch; ++b)
		{
			*cfg_and_state.output << "Assisted Excitation alpha = " << alpha << std::endl;
			Darknet::Image img = Darknet::float_to_image(l.out_w, l.out_h, 1, &gt[l.out_w*l.out_h*b]);
			char buff[100];
			sprintf(buff, "a_excitation_gt_%d", b);
			show_image_cv(img, buff);

			//image img2 = float_to_image(l.out_w, l.out_h, 1, &l.output[l.out_w*l.out_h*l.out_c*b]);
			Darknet::Image img2 = Darknet::float_to_image_scaled(l.out_w, l.out_h, 1, &l.output[l.out_w*l.out_h*l.out_c*b]);
			char buff2[100];
			sprintf(buff2, "a_excitation_output_%d", b);
			show_image_cv(img2, buff2);

			cv::waitKey(5);
		}
		cv::waitKey(0);
	}

	free(truth_cpu);
	free(gt);
	free(a_avg);
}

void pull_convolutional_layer(Darknet::Layer & l)
{
	TAT(TATPARMS);

	cuda_pull_array_async(l.weights_gpu, l.weights, l.nweights);
	cuda_pull_array_async(l.biases_gpu, l.biases, l.n);
	if (l.weight_updates_gpu) cuda_pull_array_async(l.weight_updates_gpu, l.weight_updates, l.nweights);
	if (l.bias_updates_gpu) cuda_pull_array_async(l.bias_updates_gpu, l.bias_updates, l.n);
	if (l.batch_normalize){
		cuda_pull_array_async(l.scales_gpu, l.scales, l.n);
		cuda_pull_array_async(l.rolling_mean_gpu, l.rolling_mean, l.n);
		cuda_pull_array_async(l.rolling_variance_gpu, l.rolling_variance, l.n);
	}
	if (l.adam){
		cuda_pull_array_async(l.m_gpu, l.m, l.nweights);
		cuda_pull_array_async(l.v_gpu, l.v, l.nweights);
	}
	CHECK_CUDA(cudaPeekAtLastError());
	CHECK_CUDA(cudaStreamSynchronize(get_cuda_stream()));
}

void push_convolutional_layer(Darknet::Layer & l)
{
	TAT(TATPARMS);

	cuda_push_array(l.weights_gpu, l.weights, l.nweights);
#ifdef CUDNN_HALF
	assert(l.nweights > 0);
	cuda_convert_f32_to_f16(l.weights_gpu, l.nweights, l.weights_gpu16);
#endif
	cuda_push_array(l.biases_gpu, l.biases, l.n);
	if (l.train) {
		cuda_push_array(l.weight_updates_gpu, l.weight_updates, l.nweights);
		cuda_push_array(l.bias_updates_gpu, l.bias_updates, l.n);
	}
	if (l.batch_normalize){
		cuda_push_array(l.scales_gpu, l.scales, l.n);
		cuda_push_array(l.rolling_mean_gpu, l.rolling_mean, l.n);
		cuda_push_array(l.rolling_variance_gpu, l.rolling_variance, l.n);
	}
	if (l.adam){
		cuda_push_array(l.m_gpu, l.m, l.nweights);
		cuda_push_array(l.v_gpu, l.v, l.nweights);
	}
	CHECK_CUDA(cudaPeekAtLastError());
}

void update_convolutional_layer_gpu(Darknet::Layer & l, int batch, float learning_rate_init, float momentum, float decay, float loss_scale)
{
	TAT(TATPARMS);

	if (l.deform)
	{
		if (l.rotate) rotate_weights_gpu(l.weight_updates_gpu, l.weight_deform_gpu, l.nweights, l.n, l.size, 1);
		else if (l.sway) sway_and_flip_weights_gpu(l.weight_updates_gpu, l.weight_deform_gpu, l.nweights, l.n, l.size, l.angle, 1);
		else if (l.stretch) stretch_weights_gpu(l.weight_updates_gpu, l.weight_deform_gpu, l.nweights, l.n, l.size, 0, 1);
		else if (l.stretch_sway) stretch_sway_flip_weights_gpu(l.weight_updates_gpu, l.weight_deform_gpu, l.nweights, l.n, l.size, l.angle, 1);

		reduce_and_expand_array_gpu(l.weight_deform_gpu, l.weight_updates_gpu, l.nweights, 4);
	}

	// Loss scale for Mixed-Precision on Tensor-Cores
	float learning_rate = learning_rate_init*l.learning_rate_scale / loss_scale;

	reset_nan_and_inf(l.weight_updates_gpu, l.nweights);
	fix_nan_and_inf(l.weights_gpu, l.nweights);

	// Gradient Centralization
	if (l.grad_centr && l.batch_normalize)
	{
		gradient_centralization_gpu(l.size, l.size, l.c / l.groups, l.n, l.weight_updates_gpu);
	}

	if (l.adam)
	{
		adam_update_gpu(l.weights_gpu, l.weight_updates_gpu, l.m_gpu, l.v_gpu, l.B1, l.B2, l.eps, decay, learning_rate, l.nweights, batch, l.t);

		adam_update_gpu(l.biases_gpu, l.bias_updates_gpu, l.bias_m_gpu, l.bias_v_gpu, l.B1, l.B2, l.eps, decay, learning_rate, l.n, batch, l.t);
		if (l.scales_gpu)
		{
			adam_update_gpu(l.scales_gpu, l.scale_updates_gpu, l.scale_m_gpu, l.scale_v_gpu, l.B1, l.B2, l.eps, decay, learning_rate, l.n, batch, l.t);
		}
	}
	else
	{
		float *old_weight_updates_gpu = l.weight_updates_gpu;

		if (l.reverse)
		{
			float clip = 0.0;
			float divider = 1.0;
			float abs_add = 1.0;
			mult_inverse_array_gpu(l.weight_updates_gpu, l.output_gpu, l.inputs*l.batch, l.reverse, divider, clip, abs_add);
			l.weight_updates_gpu = l.output_gpu;
		}

		axpy_ongpu(l.nweights, -decay*batch*loss_scale, l.weights_gpu, 1, l.weight_updates_gpu, 1);
		axpy_ongpu(l.nweights, learning_rate / batch, l.weight_updates_gpu, 1, l.weights_gpu, 1);

		l.weight_updates_gpu = old_weight_updates_gpu;

		scal_ongpu(l.nweights, momentum, l.weight_updates_gpu, 1);

		axpy_ongpu(l.n, learning_rate / batch, l.bias_updates_gpu, 1, l.biases_gpu, 1);
		scal_ongpu(l.n, momentum, l.bias_updates_gpu, 1);

		if (l.scales_gpu) {
			axpy_ongpu(l.n, learning_rate / batch, l.scale_updates_gpu, 1, l.scales_gpu, 1);
			scal_ongpu(l.n, momentum, l.scale_updates_gpu, 1);
		}
	}

	if (l.deform)
	{
		expand_array_gpu(l.weights_gpu, l.weight_deform_gpu, l.nweights, 4);

		if (l.rotate) rotate_weights_gpu(l.weight_deform_gpu, l.weights_gpu, l.nweights, l.n, l.size, 0);
		else if (l.sway) sway_and_flip_weights_gpu(l.weight_deform_gpu, l.weights_gpu, l.nweights, l.n, l.size, l.angle, 0);
		else if (l.stretch) stretch_weights_gpu(l.weight_deform_gpu, l.weights_gpu, l.nweights, l.n, l.size, 0, 0);
		else if (l.stretch_sway) stretch_sway_flip_weights_gpu(l.weight_deform_gpu, l.weights_gpu, l.nweights, l.n, l.size, l.angle, 0);
	}

	if (l.clip)
	{
		constrain_ongpu(l.nweights, l.clip, l.weights_gpu, 1);
	}
}
