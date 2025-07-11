#include "darknet_internal.hpp"


namespace
{
	static auto & cfg_and_state = Darknet::CfgAndState::get();
}


// ./darknet nightmare cfg/extractor.recon.cfg ~/trained/yolo-coco.conv frame6.png -reconstruct -iters 500 -i 3 -lambda .1 -rate .01 -smooth 2
//
// Note that "-i 3" parameter in the above comment, which implies 4 GPUs.  Change that to zero if you only have 1 GPU.
//
// But from the code, argv[5] must be a number.  So a better example might be:
//
// darknet nightmare cfg/yolov4-tiny.cfg yolov4-tiny.weights artwork/dog.jpg 30 -reconstruct -iters 500 -i 0 -lambda .1 -rate .01 -smooth 2
//
// See:  https://pjreddie.com/darknet/nightmare/

float abs_mean(float *x, int n)
{
	TAT(TATPARMS);

	float sum = 0.0f;
	for (int i = 0; i < n; ++i)
	{
		sum += fabs(x[i]);
	}

	return sum/n;
}


void calculate_loss(float *output, float *delta, int n, float thresh)
{
	TAT(TATPARMS);

	const float mean = mean_array(output, n);
	const float var = variance_array(output, n);

	for(int i = 0; i < n; ++i)
	{
		if(delta[i] > mean + thresh*sqrt(var))
		{
			delta[i] = output[i];
		}
		else
		{
			delta[i] = 0.0f;
		}
	}
}


void optimize_picture(Darknet::Network * net, Darknet::Image orig, int max_layer, float scale, float rate, float thresh, int norm)
{
	TAT(TATPARMS);

	//scale_image(orig, 2);
	//translate_image(orig, -1);
	net->n = max_layer + 1;

	int dx = rand()%16 - 8;
	int dy = rand()%16 - 8;
	int flip = rand()%2;

	Darknet::Image crop = Darknet::crop_image(orig, dx, dy, orig.w, orig.h);
	Darknet::Image im = Darknet::resize_image(crop, (int)(orig.w * scale), (int)(orig.h * scale));
	if (flip)
	{
		Darknet::flip_image(im);
	}

	resize_network(net, im.w, im.h);
	Darknet::Layer & last = net->layers[net->n-1];
	//net->layers[net->n - 1].activation = LINEAR;

	Darknet::Image delta = make_image(im.w, im.h, im.c);

	Darknet::NetworkState state = {0};
	state.net = *net;

#ifdef DARKNET_GPU
	state.input = cuda_make_array(im.data, im.w*im.h*im.c);
	state.delta = cuda_make_array(im.data, im.w*im.h*im.c);

	forward_network_gpu(*net, state);
	copy_ongpu(last.outputs, last.output_gpu, 1, last.delta_gpu, 1);

	cuda_pull_array(last.delta_gpu, last.delta, last.outputs);
	calculate_loss(last.delta, last.delta, last.outputs, thresh);
	cuda_push_array(last.delta_gpu, last.delta, last.outputs);

	backward_network_gpu(*net, state);

	cuda_pull_array(state.delta, delta.data, im.w*im.h*im.c);
	cuda_free(state.input);
	cuda_free(state.delta);
#else
	state.input = im.data;
	state.delta = delta.data;
	forward_network(*net, state);
	copy_cpu(last.outputs, last.output, 1, last.delta, 1);
	calculate_loss(last.output, last.delta, last.outputs, thresh);
	backward_network(*net, state);
#endif

	if (flip)
	{
		Darknet::flip_image(delta);
	}

	//normalize_array(delta.data, delta.w*delta.h*delta.c);
	Darknet::Image resized = Darknet::resize_image(delta, orig.w, orig.h);
	Darknet::Image out = Darknet::crop_image(resized, -dx, -dy, orig.w, orig.h);

	if(norm)
	{
		normalize_array(out.data, out.w*out.h*out.c);
	}
	axpy_cpu(orig.w*orig.h*orig.c, rate, out.data, 1, orig.data, 1);

	/*
	normalize_array(orig.data, orig.w*orig.h*orig.c);
	scale_image(orig, sqrt(var));
	translate_image(orig, mean);
	*/

	//translate_image(orig, 1);
	//scale_image(orig, .5);
	//normalize_image(orig);

	Darknet::constrain_image(orig);

	Darknet::free_image(crop);
	Darknet::free_image(im);
	Darknet::free_image(delta);
	Darknet::free_image(resized);
	Darknet::free_image(out);

}


void smooth(Darknet::Image recon, Darknet::Image update, float lambda, int num)
{
	TAT(TATPARMS);

	for (int k = 0; k < recon.c; ++k)
	{
		for (int j = 0; j < recon.h; ++j)
		{
			for (int i = 0; i < recon.w; ++i)
			{
				int out_index = i + recon.w*(j + recon.h*k);
				for (int jj = j-num; jj <= j + num && jj < recon.h; ++jj)
				{
					if (jj < 0)
					{
						continue;
					}

					for (int ii = i-num; ii <= i + num && ii < recon.w; ++ii)
					{
						if (ii < 0)
						{
							continue;
						}

						int in_index = ii + recon.w*(jj + recon.h*k);
						update.data[out_index] += lambda * (recon.data[in_index] - recon.data[out_index]);
					}
				}
			}
		}
	}
}


void reconstruct_picture(Darknet::Network net, float *features, Darknet::Image recon, Darknet::Image update, float rate, float momentum, float lambda, int smooth_size, int iters)
{
	TAT(TATPARMS);

	for (int iter = 0; iter < iters; ++iter)
	{
		Darknet::Image delta = make_image(recon.w, recon.h, recon.c);

		Darknet::NetworkState state = {0};
		state.net = net;

#ifdef DARKNET_GPU
		state.input = cuda_make_array(recon.data, recon.w*recon.h*recon.c);
		state.delta = cuda_make_array(delta.data, delta.w*delta.h*delta.c);
		state.truth = cuda_make_array(features, get_network_output_size(net));

		forward_network_gpu(net, state);
		backward_network_gpu(net, state);

		cuda_pull_array(state.delta, delta.data, delta.w*delta.h*delta.c);

		cuda_free(state.input);
		cuda_free(state.delta);
		cuda_free(state.truth);
#else
		state.input = recon.data;
		state.delta = delta.data;
		state.truth = features;

		forward_network(net, state);
		backward_network(net, state);
#endif

		axpy_cpu(recon.w*recon.h*recon.c, 1, delta.data, 1, update.data, 1);
		smooth(recon, update, lambda, smooth_size);

		axpy_cpu(recon.w*recon.h*recon.c, rate, update.data, 1, recon.data, 1);
		scal_cpu(recon.w*recon.h*recon.c, momentum, update.data, 1);

		//float mag = mag_array(recon.data, recon.w*recon.h*recon.c);
		//scal_cpu(recon.w*recon.h*recon.c, 600/mag, recon.data, 1);

		Darknet::constrain_image(recon);
		Darknet::free_image(delta);
	}
}


void run_nightmare(int argc, char **argv)
{
	TAT(TATPARMS);

	if (argc < 4)
	{
		*cfg_and_state.output << "Usage: " << argv[0] << " " << argv[1] << " [cfg] [weights] [image] [layer] [options! (optional)]" << std::endl;
		return;
	}

	char *cfg = argv[2];
	char *weights = argv[3];
	char *input = argv[4];
	int max_layer = atoi(argv[5]);	// obviously it is expecting a number for argv[5]?

	int range = find_int_arg(argc, argv, "-range", 1);
	int norm = find_int_arg(argc, argv, "-norm", 1);
	int rounds = find_int_arg(argc, argv, "-rounds", 1);
	int iters = find_int_arg(argc, argv, "-iters", 10);
	int octaves = find_int_arg(argc, argv, "-octaves", 4);
	float zoom = find_float_arg(argc, argv, "-zoom", 1.);
	float rate = find_float_arg(argc, argv, "-rate", .04);
	float thresh = find_float_arg(argc, argv, "-thresh", 1.);
	float rotate = find_float_arg(argc, argv, "-rotate", 0);
	float momentum = find_float_arg(argc, argv, "-momentum", .9);
	float lambda = find_float_arg(argc, argv, "-lambda", .01);
	const char *prefix = find_char_arg(argc, argv, "-prefix", 0);
	int reconstruct = find_arg(argc, argv, "-reconstruct");
	int smooth_size = find_int_arg(argc, argv, "-smooth", 1);

	cuda_set_device(0);

	Darknet::Network net = parse_network_cfg(cfg);
	load_weights(&net, weights);
	const char *cfgbase = basecfg(cfg);
	const char *imbase = basecfg(input);

	set_batch_network(&net, 1);
	Darknet::Image im = Darknet::load_image(input, 0, 0, net.c);

	Darknet::show_image(im, "original image");

#if 0
	if (0)
	{
		float scale = 1;
		if (im.w > 512 || im.h > 512)
		{
			if(im.w > im.h)
			{
				scale = 512.0/im.w;
			}
			else
			{
				scale = 512.0/im.h;
			}
		}

		image resized = Darknet::resize_image(im, scale*im.w, scale*im.h);
		free_image(im);
		im = resized;
	}
#endif

	float *features = 0;
	Darknet::Image update;
	if (reconstruct)
	{
		resize_network(&net, im.w, im.h);

		int zz = 0;
		network_predict(net, im.data);
		Darknet::Image out_im = get_network_image(net);
		Darknet::Image crop = Darknet::crop_image(out_im, zz, zz, out_im.w-2*zz, out_im.h-2*zz);
		Darknet::Image f_im = Darknet::resize_image(crop, out_im.w, out_im.h);
		Darknet::free_image(crop);
		*cfg_and_state.output << (out_im.w * out_im.h * out_im.c) << " features" << std::endl;

		/// @todo Is there a memory leak here because we don't free the original im?
		im = Darknet::resize_image(im, im.w, im.h);
		f_im = Darknet::resize_image(f_im, f_im.w, f_im.h);
		features = f_im.data;

		for(int i = 0; i < 14*14*512; ++i)
		{
			features[i] += rand_uniform(-0.19f, 0.19f);
		}

		Darknet::free_image(im);
		im = Darknet::make_random_image(im.w, im.h, im.c);
		update = make_image(im.w, im.h, im.c);
	}

	for (int e = 0; e < rounds; ++e)
	{
		*cfg_and_state.output << "Iteration: ";
		for (int n = 0; n < iters; ++n)
		{
			*cfg_and_state.output << n << ", " << std::flush;

			if (reconstruct)
			{
				reconstruct_picture(net, features, im, update, rate, momentum, lambda, smooth_size, 1);
				//if ((n+1)%30 == 0) rate *= .5;
			}
			else
			{
				int layer = max_layer + rand()%range - range/2;
				int octave = rand()%octaves;
				optimize_picture(&net, im, layer, 1/pow(1.33333333, octave), rate, thresh, norm);
			}
		}
		*cfg_and_state.output << "done!" << std::endl;

		if (0)
		{
			Darknet::Image g = Darknet::grayscale_image(im);
			Darknet::free_image(im);
			im = g;
		}

		char buff[256];
		if (prefix)
		{
			sprintf(buff, "%s/%s_%s_%d_%06d", prefix, imbase, cfgbase, max_layer, e);
		}
		else
		{
			sprintf(buff, "%s_%s_%d_%06d", imbase, cfgbase, max_layer, e);
		}
		*cfg_and_state.output << e << " " << buff << std::endl;
		Darknet::save_image(im, buff);

		if (rotate)
		{
			Darknet::Image rot = Darknet::rotate_image(im, rotate);
			Darknet::free_image(im);
			im = rot;
		}

		Darknet::Image crop = Darknet::crop_image(im, im.w * (1. - zoom)/2., im.h * (1.-zoom)/2., im.w*zoom, im.h*zoom);
		Darknet::Image resized = Darknet::resize_image(crop, im.w, im.h);
		Darknet::free_image(im);
		Darknet::free_image(crop);
		im = resized;

		Darknet::show_image(im, "results");
		cv::waitKey(20);
	}

	// show the last results until the user pushes a key
	cv::waitKey(-1);
}
