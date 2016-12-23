#include <math.h>
#include <stdio.h>
#include <assert.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include "kann.h"

typedef struct {
	int len, n_char;
	uint8_t *data;
	int c2i[256];
} tg_data_t;

#define kv_roundup32(x) (--(x), (x)|=(x)>>1, (x)|=(x)>>2, (x)|=(x)>>4, (x)|=(x)>>8, (x)|=(x)>>16, ++(x))

uint8_t *tg_read_file(const char *fn, int *_len)
{
	const int buf_len = 0x10000;
	int len = 0, max = 0, l;
	FILE *fp;
	uint8_t *buf, *s = 0;

	fp = fn && strcmp(fn, "-")? fopen(fn, "rb") : stdin;
	buf = (uint8_t*)malloc(buf_len);
	while ((l = fread(buf, 1, buf_len, fp)) > 0) {
		if (len + l > max) {
			max = len + buf_len;
			kv_roundup32(max);
			s = (uint8_t*)realloc(s, max);
		}
		memcpy(&s[len], buf, l);
		len += l;
	}
	s = (uint8_t*)realloc(s, len);
	*_len = len;
	fclose(fp);
	free(buf);
	return s;
}

tg_data_t *tg_read(const char *fn)
{
	int i, j;
	tg_data_t *tg;
	tg = (tg_data_t*)calloc(1, sizeof(tg_data_t));
	tg->data = tg_read_file(fn, &tg->len);
	for (i = 0; i < tg->len; ++i)
		tg->c2i[tg->data[i]] = 1;
	for (i = j = 0; i < 256; ++i)
		if (tg->c2i[i] == 0) tg->c2i[i] = -1;
		else tg->c2i[i] = j++;
	tg->n_char = j;
	for (i = 0; i < tg->len; ++i)
		tg->data[i] = tg->c2i[tg->data[i]];
	return tg;
}

void tg_save(const char *fn, kann_t *ann, int map[256])
{
	FILE *fp;
	fp = fn && strcmp(fn, "-")? fopen(fn, "wb") : stdout;
	kann_save_fp(fp, ann);
	fwrite(map, sizeof(int), 256, fp);
	fclose(fp);
}

kann_t *tg_load(const char *fn, int map[256])
{
	FILE *fp;
	kann_t *ann;
	fp = fn && strcmp(fn, "-")? fopen(fn, "rb") : stdin;
	ann = kann_load_fp(fp);
	fread(map, sizeof(int), 256, fp);
	fclose(fp);
	return ann;
}

void tg_train(kann_t *ann, float lr, int ulen, int mbs, int max_epoch, int len, const uint8_t *data)
{
	int i, k, n_var, n_char;
	float **x, **y, *r, *g;
	kann_t *ua;

	n_char = kann_n_in(ann);
	x = (float**)calloc(ulen, sizeof(float*));
	y = (float**)calloc(ulen, sizeof(float*));
	for (k = 0; k < ulen; ++k) {
		x[k] = (float*)calloc(n_char, sizeof(float));
		y[k] = (float*)calloc(n_char, sizeof(float));
	}
	n_var = kad_n_var(ann->n, ann->v);
	r = (float*)calloc(n_var, sizeof(float));
	g = (float*)calloc(n_var, sizeof(float));

	ua = kann_unroll(ann, ulen);
	kann_bind_feed(ua, KANN_F_IN,    0, x);
	kann_bind_feed(ua, KANN_F_TRUTH, 0, y);
	for (i = 0; i < max_epoch; ++i) {
		double cost = 0.0;
		int j, b, tot = 0;
		for (j = 1; j + ulen * mbs - 1 < len; j += ulen * mbs) {
			memset(g, 0, n_var * sizeof(float));
			for (b = 0; b < mbs; ++b) { // a mini-batch
				for (k = 0; k < ulen; ++k) {
					memset(x[k], 0, n_char * sizeof(float));
					memset(y[k], 0, n_char * sizeof(float));
					x[k][data[j+b*ulen+k-1]] = 1.0f;
					y[k][data[j+b*ulen+k]] = 1.0f;
				}
				cost += kann_cost(ua, 1) * ulen;
				tot += ulen;
				for (k = 0; k < n_var; ++k) g[k] += ua->g[k];
				#if 1
				for (k = 0; k < ua->n; ++k) // keep the cycle rolling
					if (ua->v[k]->pre)
						memcpy(ua->v[k]->pre->x, ua->v[k]->x, kad_len(ua->v[k]) * sizeof(float));
				#endif
			}
			for (k = 0; k < n_var; ++k) g[k] /= mbs;
			kann_RMSprop(n_var, lr, 0, 0.9f, g, ua->x, r);
		}
		fprintf(stderr, "epoch: %d; running cost: %g\n", i+1, cost / tot);
		kann_save("t.knm", ann);
	}
	kann_delete_unrolled(ua);

	for (k = 0; k < ulen; ++k) {
		free(x[k]);
		free(y[k]);
	}
	free(g); free(r); free(y); free(x);
}

static kann_t *model_gen(int model, int n_char, int n_h_layers, int n_h_neurons, float h_dropout)
{
	int i;
	kad_node_t *t;
	t = kann_layer_input(n_char);
	for (i = 0; i < n_h_layers; ++i) {
		if (model == 0) t = kann_layer_rnn(t, n_h_neurons, kad_tanh);
		else if (model == 1) t = kann_layer_lstm(t, n_h_neurons);
		else if (model == 2) t = kann_layer_gru(t, n_h_neurons);
		t = kann_layer_dropout(t, h_dropout);
	}
	return kann_layer_final(t, n_char, KANN_C_CEM);
}

int main(int argc, char *argv[])
{
	int i, c, seed = 11, ulen = 40, n_h_layers = 1, n_h_neurons = 128, model = 0, max_epoch = 20, mbs = 64, c2i[256];
	float h_dropout = 0.0f, temp = 0.5f, lr = 0.01f;
	kann_t *ann = 0;
	char *fn_in = 0, *fn_out = 0;

	while ((c = getopt(argc, argv, "n:l:s:r:m:B:o:i:d:b:T:M:u:")) >= 0) {
		if (c == 'n') n_h_neurons = atoi(optarg);
		else if (c == 'l') n_h_layers = atoi(optarg);
		else if (c == 's') seed = atoi(optarg);
		else if (c == 'i') fn_in = optarg;
		else if (c == 'o') fn_out = optarg;
		else if (c == 'r') lr = atof(optarg);
		else if (c == 'm') max_epoch = atoi(optarg);
		else if (c == 'B') mbs = atoi(optarg);
		else if (c == 'd') h_dropout = atof(optarg);
		else if (c == 'T') temp = atof(optarg);
		else if (c == 'u') ulen = atoi(optarg);
		else if (c == 'M') {
			if (strcmp(optarg, "rnn") == 0) model = 0;
			else if (strcmp(optarg, "lstm") == 0) model = 1;
			else if (strcmp(optarg, "gru") == 0) model = 2;
		}
	}
	if (argc == optind && fn_in == 0) {
		FILE *fp = stdout;
		fprintf(fp, "Usage: textgen [options] <in.fq>\n");
		fprintf(fp, "Options:\n");
		fprintf(fp, "  Model construction:\n");
		fprintf(fp, "    -i FILE     read trained model from FILE []\n");
		fprintf(fp, "    -o FILE     save trained model to FILE []\n");
		fprintf(fp, "    -s INT      random seed [%d]\n", seed);
		fprintf(fp, "    -l INT      number of hidden layers [%d]\n", n_h_layers);
		fprintf(fp, "    -n INT      number of hidden neurons per layer [%d]\n", n_h_neurons);
		fprintf(fp, "    -M STR      model: rnn, lstm or gru [rnn]\n");
		fprintf(fp, "  Model training:\n");
		fprintf(fp, "    -r FLOAT    learning rate [%g]\n", lr);
		fprintf(fp, "    -d FLOAT    dropout at the hidden layer(s) [%g]\n", h_dropout);
		fprintf(fp, "    -m INT      max number of epochs [%d]\n", max_epoch);
		fprintf(fp, "    -B INT      mini-batch size [%d]\n", mbs);
		fprintf(fp, "    -u INT      max unroll [%d]\n", ulen);
		fprintf(fp, "  Text generation:\n");
		fprintf(fp, "    -T FLOAT    temperature [%g]\n", temp);
		return 1;
	}

	kann_srand(seed);
	kad_trap_fe();
	if (fn_in) ann = tg_load(fn_in, c2i);

	if (argc - optind >= 1) { // train
		tg_data_t *tg;
		tg = tg_read(argv[optind]);
		fprintf(stderr, "Read %d characters; alphabet size %d\n", tg->len, tg->n_char);
		if (!ann) ann = model_gen(model, tg->n_char, n_h_layers, n_h_neurons, h_dropout);
		tg_train(ann, lr, ulen, mbs, max_epoch, tg->len, tg->data);
		if (fn_out) tg_save(fn_out, ann, tg->c2i);
		free(tg->data); free(tg);
	} else { // apply
		int n_char, i2c[256];
		memset(i2c, 0, 256 * sizeof(int));
		for (i = 0; i < 256; ++i)
			if (c2i[i] >= 0) i2c[c2i[i]] = i;
		n_char = kann_n_in(ann);
		kann_set_scalar(ann, KANN_F_TEMP, 1.0f/temp);
		kann_rnn_start(ann);
		c = (int)(n_char * kann_drand());
		for (i = 0; i < 1000; ++i) {
			float x[256], s, r;
			const float *y;
			memset(x, 0, n_char * sizeof(float));
			x[c] = 1.0f;
			y = kann_apply1(ann, x);
			r = kann_drand();
			for (c = 0, s = 0.0f; c < n_char; ++c)
				if (s + y[c] >= r) break;
				else s += y[c];
			putchar(i2c[c]);
		}
		putchar('\n');
		kann_rnn_end(ann);
	}

	kann_delete(ann);
	return 0;
}