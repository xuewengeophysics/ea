#include <ucsl.h>
#include <sdw_obj.h>
#include <sdw_slope.h>
#include <cart_volume_io.h>
#include <ctime>
#ifdef _USE_OMP
#include <omp.h>
#endif
#include <mpi.h>

char *usage[] = {
  "************************************************************",
  " ufilter par=[key=val]",
  " Parameters:",
  " in=",
  " out=",
  " q:=",
  " k=10",
  " pmax=4.0",
  " h1=2",
  " h2=2",
  " h3=2",
  " r1=0.1",
  " r2=0.3",
  " r3=0.3",
  " niter=1",
  " nthread=number of cores",
  " mask_val=1",
  " mask_option=eq (eq,lt,le,gt,ge)",
  "************************************************************",
  NULL};

typedef struct {
  char *fn_in;
  char *fn_out;
  char *fn_qc;
  char *mask_option;
  file_trace *fd_in;
  axis **axes_in;
  int k;  // slope values are computed in increments of 1/k
  int h1; // subsample factor in depth dimension
  int h2; // subsample factor in xline dimension
  int h3; // subsample factor in inline/subline dimension
  int pad3;
  int mype;
  int niter; 
  int naxes;
  int nthread;
  int verbose;
  double r1; // max strain in depth dimension
  double r2; // max strain in xline dimension
  double r3; // max strain in inline/subline dimension
  double pmax; //max slope (samples/trace)
  float mask_val;
} prog_data;

int init_cart_volume(cart_volume **vol_out, axis **axes_in, 
    size_t const& naxes, int h1, int h2, int h3, MPI_Comm comm);
void do_filter2d(prog_data *pd, parlist *par); 
void do_filter3d(prog_data *pd, parlist *par); 

int main (int argc, char *argv[]) {
  double global_time_start, global_time_end;
  prog_data *pd;
  parlist *par;

  par = ucsl_initialize(usage,argv,argc,1);
  if(!par->get_status()) return _ERROR_PAR;
  global_time_start = MPI_Wtime();

  pd = (prog_data*)mem_alloc(sizeof(prog_data));
  pd->mype = ucsl_mype();

  par->get_string_req("in", &(pd->fn_in));
  par->get_string_req("out",&(pd->fn_out));
  pd->fn_qc = NULL;
  par->get_string("qc", &(pd->fn_qc));
  par->get_double_def("pmax",&(pd->pmax),4.0);
  if (pd->pmax <= 0.0f) {
    ucsl_errorf("ERROR: pmax must be positive, read %f.\n",pd->pmax);
  }
  par->get_int_def("k",&(pd->k),10);
  if (pd->k <= 0) {
    ucsl_errorf("ERROR: k must be positive, read %d.\n",pd->k);
  }
  par->get_int_def("h1",&(pd->h1),2);
  if (pd->h1 <= 1) {
    ucsl_errorf("ERROR: h1 must be greater than 1, read %d.\n",pd->h1);
  }
  par->get_int_def("h2",&(pd->h2),2);
  if (pd->h2 <= 1) {
    ucsl_errorf("ERROR: h2 must be greater than 1, read %d.\n",pd->h2);
  }
  par->get_int_def("h3",&(pd->h3),2);
  if (pd->h3 <= 1) {
    ucsl_errorf("ERROR: h3 must be greater than 1, read %d.\n",pd->h3);
  }
  par->get_double_def("r1",&(pd->r1),0.1);
  if (pd->r1 <= 0.0f) {
    ucsl_errorf("ERROR: r1 must be positive, read %f.\n",pd->r1);
  }
  par->get_double_def("r2",&(pd->r2),0.3);
  if (pd->r2 <= 0.0f) {
    ucsl_errorf("ERROR: r2 must be positive, read %f.\n",pd->r2);
  }
  par->get_double_def("r3",&(pd->r3),0.3);
  if (pd->r3 <= 0.0f) {
    ucsl_errorf("ERROR: r3 must be positive, read %f.\n",pd->r3);
  }
  par->get_int_def("pad_subline_multiplier",&(pd->pad3),1);
  if (pd->pad3 <= 0) {
    ucsl_errorf("ERROR: pad3 must be positive, read %d.\n",pd->pad3);
  }
  pd->pad3 *= pd->h3;
  par->get_int_def("nthread",&(pd->nthread),ucsl_get_n_cores());
  par->get_int_def("verbose",&(pd->verbose),1); 
  par->get_string_def("mask_option",&(pd->mask_option),"eq");
  par->get_float_def("mask_val",&(pd->mask_val),1.0f);
  if(pd->verbose && !pd->mype) {
    ucsl_print("**********************************************************\n");
    ucsl_printf("sdw version %s\n",_UCSL_VERSION);
    par->print(_UCSL_LOG);
    ucsl_printf("using %d threads\n",pd->nthread);
    ucsl_print("**********************************************************\n");
  }
#ifdef _USE_OMP
  omp_set_num_threads(pd->nthread);
#endif 
  ucsl_abort_if_error();
  pd->fd_in = new file_trace(pd->fn_in, _FILE_READ);
  ucsl_abort_if_error();
  pd->naxes = pd->fd_in->get_axes(&(pd->axes_in));

  if(pd->naxes == 3 && pd->axes_in[2]->n > 1) do_filter3d(pd, par);
  else if((pd->naxes == 2 || (pd->naxes == 3 && pd->axes_in[2]->n == 1)) && 
      !pd->mype) do_filter2d(pd, par);

  delete pd->fd_in;
  delete par;

  global_time_end = MPI_Wtime();
  double const global_elapsed_time = global_time_end - global_time_start;
  ucsl_printf("(PE: %d) Elapsed time: %.3f\n",pd->mype,global_elapsed_time);
  ucsl_finalize();
  return EXIT_SUCCESS;
}

void do_filter2d(prog_data *pd, parlist *par) {
  file_trace *fd_out;
  hdr_map *hdr;
  float **tbuf, **dbuf;
  float **data, **slopes;
  axis *axs1, *axs2;
  int i, j;

  int bytes_trace = pd->fd_in->get_bytes_trace();
  int n1 = pd->axes_in[0]->n;
  int n2 = pd->axes_in[1]->n;

  axs1 = new axis(0.0f,1.0f,n1); // axis for shifts in 1st dimension
  axs2 = new axis(0.0f,1.0f,n2); // axis for shifts in 2nd dimension

  tbuf   = (float**)mem_alloc2(bytes_trace,n2,1);
  data   = (float**)mem_alloc2(n1,n2,sizeof(float));
  slopes = (float**)mem_alloc2(n1,n2,sizeof(float));

  hdr = pd->fd_in->get_hdr_map();
  if(hdr) dbuf = hdr->get_data_ptr(tbuf,n2);
  else dbuf = tbuf;

  pd->fd_in->read_traces(tbuf[0],n2,0);

  for(j=0; j<n2; ++j) {
    for(i=0; i<n1; ++i) {
      data[j][i] = dbuf[j][i];
    }
  }

  //Smooth dynamic warping routine
  sdw_slope *sdws = new sdw_slope(pd->k,pd->pmax,pd->h1,pd->h2,pd->r1,pd->r2,
      axs1,axs2);
  sdws->setErrorSmoothing(1);
  sdws->findSlopes(axs1,data,slopes);

  for(j=0; j<n2; ++j) {
    for(i=0; i<n1; ++i) {
      dbuf[j][i] = slopes[j][i];
    }
  }
  fd_out = new file_trace(pd->fd_in,pd->fn_out,NULL);
  fd_out->write_traces(tbuf[0],n2,0);
  delete fd_out;

  mem_free2((void***)&tbuf);
}


void do_filter3d(prog_data *pd, parlist *par) {
  file_trace *fd_out_slopex, *fd_out_slopey;
  cart_volume *vol(0);
  char *hdr_buffer(0);
  char *fn_out_slopex, *fn_out_slopey;
  int res;
  axis *ax1, *ax2, *ax3;
  axis *axs1, *axs2, *axs3;
  float ***slopex, ***slopey;

  res = init_cart_volume(&vol,pd->axes_in,pd->naxes,0,0,pd->pad3,MPI_COMM_WORLD);
  if(res) ucsl_errorf("(me: %d) Failed to initialize cart_volume\n",pd->mype);
  ucsl_abort_if_error();

  res = fill_cart_volume(vol,&hdr_buffer,pd->fd_in,MPI_COMM_WORLD);
  if(res) ucsl_errorf("(me: %d) Failed to read input\n",pd->mype);
  ucsl_abort_if_error();

  vol->extend_axes();
  vol->update_edges();

  fn_out_slopex = (char*)calloc(strlen(pd->fn_out)+6,sizeof(char));
  fn_out_slopey = (char*)calloc(strlen(pd->fn_out)+6,sizeof(char));
  sprintf(fn_out_slopex,"%s_dzdx",pd->fn_out);
  sprintf(fn_out_slopey,"%s_dzdy",pd->fn_out);
  fd_out_slopex = new file_trace(pd->fd_in,fn_out_slopex,MPI_COMM_WORLD);
  fd_out_slopey = new file_trace(pd->fd_in,fn_out_slopey,MPI_COMM_WORLD);

  ax1 = vol->ax1;
  ax2 = vol->ax2;
  ax3 = vol->ax3;
  size_t n1 = ax1->ntot;
  size_t n2 = ax2->ntot;
  size_t n3 = ax3->ntot;
  ucsl_printf("n1=%ld %ld %ld\n",n1,n2,n3);
  axs1 = new axis(0.0f,1.0f,static_cast<int>(n1));
  axs2 = new axis(0.0f,1.0f,static_cast<int>(n2));
  axs3 = new axis(0.0f,1.0f,static_cast<int>(n3));
  slopex = (float***)mem_alloc3(n1,n2,n3,sizeof(float));
  slopey = (float***)mem_alloc3(n1,n2,n3,sizeof(float));

  //Smooth dynamic warping routine
  sdw_slope *sdws = new sdw_slope(pd->k,pd->pmax,pd->h1,pd->h2,pd->h3,
      pd->r1,pd->r2,pd->r3,axs1,axs2,axs3);
  sdws->setErrorSmoothing(1);
  sdws->findSlopes(axs1,vol->data,slopex,slopey);

  memcpy(vol->data[0][0],slopex[0][0],n1*n2*n3*sizeof(float));
  mem_free3((void****)&slopex);
  res = dump_cart_volume(fd_out_slopex,vol,hdr_buffer,MPI_COMM_WORLD);
  if(res) ucsl_errorf("(PE: %d) Failed to write filtered output!\n",pd->mype);
  delete fd_out_slopex;

  memcpy(vol->data[0][0],slopey[0][0],n1*n2*n3*sizeof(float));
  mem_free3((void****)&slopey);
  res = dump_cart_volume(fd_out_slopey,vol,hdr_buffer,MPI_COMM_WORLD);
  if(res) ucsl_errorf("(PE: %d) Failed to write filtered output!\n",pd->mype);
  delete fd_out_slopey;

  delete axs1;
  delete axs2;
  delete axs3;
  free(fn_out_slopex);
  free(fn_out_slopey);

  delete vol;
  if(hdr_buffer) mem_free((void **)&hdr_buffer);
}

int init_cart_volume(
    cart_volume ** vol_out, axis **axes_in, size_t const& naxes, 
    int h1, int h2, int h3, MPI_Comm comm) {
  axis **axes_filter = new axis*[naxes];
  int filter_hl_list[3];
  filter_hl_list[0] = h1;
  filter_hl_list[1] = h2;
  filter_hl_list[2] = h3;

  for(size_t i(0); i < naxes; ++i) {
    axes_filter[i] = new axis(
        axes_in[i]->o,axes_in[i]->d,axes_in[i]->n,filter_hl_list[i],0);
    axes_filter[i]->set_label(axes_in[i]->label);
  }

  int const mype = ucsl_mype();
  int const npe = ucsl_npe();

  int const estimated_distribution =
    cart_volume::compute_n(axes_filter[2]->n, mype, npe);

  if(estimated_distribution <= filter_hl_list[2]) {
    ucsl_errorf("The ghost region is too small.\n");
    return 1;
  }

  (*vol_out) = new cart_volume(
      axes_filter[0],axes_filter[1],axes_filter[2],comm);
  return 0;
}
