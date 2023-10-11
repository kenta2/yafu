/*----------------------------------------------------------------------
This source distribution is placed in the public domain by its author,
Ben Buhrow. You may use it for any purpose, free of charge,
without having to notify anyone. I disclaim any responsibility for any
errors.

Optionally, please be nice and tell me if you find this source to be
useful. Again optionally, if you add to the functionality present here
please consider making those additions public too, so that others may 
benefit from your work.	

Some parts of the code (and also this header), included in this 
distribution have been reused from other sources. In particular I 
have benefitted greatly from the work of Jason Papadopoulos's msieve @ 
www.boo.net/~jasonp, Scott Contini's mpqs implementation, and Tom St. 
Denis Tom's Fast Math library.  Many thanks to their kind donation of 
code to the public domain.
       				   --bbuhrow@gmail.com 11/24/09
----------------------------------------------------------------------*/

#include "qs_impl.h"
#include "ytools.h"
#include "common.h"
#include "poly_macros_common.h"
#include "poly_macros_32k.h"

#define FILL_ONE_PRIME_LOOP_P_test(i)				\
	bnum = root1 >> 15;					\
	while (bnum < numblocks)					\
	{											\
		bptr = sliceptr_p +						\
			(bnum << BUCKET_BITS) +				\
			numptr_p[bnum];					\
		*bptr = (((i) - bound_val) << 16 | (root1 & 32767)); \
		numptr_p[bnum]++;					\
		root1 += prime;							\
		bnum = root1 >> 15;				\
	}											\

#ifdef USE_SS_SEARCH
#define SS_TIMING
#endif

void testfirstRoots_32k(static_conf_t *sconf, dynamic_conf_t *dconf)
{
	// the roots are computed using a and b as follows:
	// (+/-t - b)(a)^-1 mod p
	// where the t values are the roots to t^2 = N mod p, found by shanks_tonelli
	// when constructing the factor base.
	// assume b > t
       
	// compute the roots as if we were actually going to use this, but don't save
	// anything.  We are just trying to determine the size needed for each large 
	// prime bucket by sieving over just the first bucket

	uint32_t i,logp;
	int root1, root2, prime, amodp, bmodp, inv, bnum,numblocks;
	int lpnum,last_bound;

	// unpack stuff from the job data
	siqs_poly *poly = dconf->curr_poly;
	fb_list *fb = sconf->factor_base;
	lp_bucket *lp_bucket_p = dconf->buckets;
	uint32_t *modsqrt = sconf->modsqrt_array;

	numblocks = sconf->num_blocks;

	lpnum = 0;
	dconf->buckets->alloc_slices = 1;

	// extreme estimate for number of slices
	i = (sconf->factor_base->B - sconf->factor_base->med_B) / 512;

	last_bound = fb->med_B;
	for (i=fb->med_B;i<fb->B;i++)
	{
		prime = fb->list->prime[i];
		root1 = modsqrt[i]; 
		root2 = prime - root1; 
		logp = fb->list->logprime[i];

		amodp = (int)mpz_tdiv_ui(poly->mpz_poly_a,prime);
		bmodp = (int)mpz_tdiv_ui(poly->mpz_poly_b,prime);

		//find a^-1 mod p = inv(a mod p) mod p
		inv = modinv_1(amodp,prime);

		root1 = (int)root1 - bmodp;
		if (root1 < 0) root1 += prime;

		root2 = (int)root2 - bmodp;
		if (root2 < 0) root2 += prime;
	
		root1 = (uint32_t)((uint64_t)inv * (uint64_t)root1 % (uint64_t)prime);
		root2 = (uint32_t)((uint64_t)inv * (uint64_t)root2 % (uint64_t)prime);

		// just need to do this once, because the next step of prime will be 
		// into a different bucket
		bnum = root1 >> 15;
		if (bnum == 0)
			lpnum++;

		// repeat for the other root
		bnum = root2 >> 15;
		if (bnum == 0)
			lpnum++;

		if ((uint32_t)lpnum > (double)BUCKET_ALLOC * 0.75)
		{
			// we want to allocate more slices than we will probably need
			// assume alloc/2 is a safe amount of slack
			lp_bucket_p->alloc_slices++;
			lpnum = 0;
		}

		if (i - last_bound == 65536)
		{
			// when prime are really big, we may cross this boundary
			// before the buckets fill up
			lp_bucket_p->alloc_slices++;
			lpnum = 0;
			last_bound = i;
		}
	}

	// extra cushion - may increase the memory usage a bit, but in very
	// rare circumstances not enough slices allocated causes crashes.
	lp_bucket_p->alloc_slices++;

#ifdef DO_VLP_OPT
    // one more slice because we force a new one at the VLP boundary.
    lp_bucket_p->alloc_slices++;
#endif

	return;
}

// Subset-sum notes:
// siqs initialization amounts to fast ways to compute 
// x == a^-1 * (+/- t - (+/- B1 +/- B2 +/- B3 ... + Bs) mod p) mod p
// for all of the combinations of the B's, for each p.
// jasonp's subset-sum search method uses a time/space tradeoff to
// the sum.  With 's' B-values there are 2^(s-1) combinations of sums of B's.
// Instead of enumerating all of the sum combinations we split the list of B's
// and enumerate each half mod p (including the a^-1 and +/- t on one side).
// (sqrt factor of the number of sum enumerations.)
// These are set1 and set2.
// Now hash each set into a group of bins along the interval 0:prime, so
// that each enumeration mod p gets grouped with others about the same size.
// (sorting, essentially.  perhaps a sort would even be faster than a hash.)
// (... it doesn't matter much, this part of the algorithm is not the bottlenectk.)
// For each bin in set 1, compute the center of the set, x, and add each
// member of that bin to the bin in set 2 that contains prime - x.  So that
// the x's mostly cancel and we are left with something near in value to prime, 
// which taken mod p is near 0, i.e., likely to be very small and thus produce a 
// sieve hit for the polynomial represented by the enumeration in set1+set2.
// (Note that there are also hits when combining bin x with (p-x)+c and
// when combining bin x with (p-x)-c, for some value of 'c', depending on the size
// of the bins and simply because some binned values lie near the edge of the bin.)
// finally there is a choice of what to do with the resulting sieve hits.
// We could 1) sort the list of all sieve hits by their associated enumeration #,
// so that we can sieve "normally" by all small primes for each polynomial, and
// afterwards dump in the large prime sieve hits for that polynomial.  
// Sorting could occur using 
// 1a) a literal sort: put the sieve hits into a giant linear array as we
// find them and then sort it.
// 1b) a bucket sort: insert each sieve hit into its appropriate polynomial-bin
// as we find them.
// 2) put the sieve hits into linked lists (per polynomial) and traverse the lists
// when sieving.
// Note that we can still use all of the traditional sieve algorithms that
// yafu uses, including bucket sieving.  This new subset-sum technique would
// take over at some large factor base offset, replacing the bucket sieve for
// the largest set of fb primes.  Then we just have to find some parameterization
// (fb size, interval size, td cutoffs, SS start offset, etc.) that hopefully
// results in an overall faster algorithm for usefully sized N (i.e., smaller
// than the current qs-gnfs crossover).


#if defined( USE_POLY_BUCKET_SS ) && defined(USE_AVX512F)

#define TRY_PN_BUCKET_COMBINE
#define TRY_GATHER_SCATTER

void ss_search_clear(static_conf_t* sconf, dynamic_conf_t* dconf)
{
	int i;

	mpz_clear(dconf->polyb1);
	mpz_clear(dconf->polyb2);
	free(dconf->polyv);
	free(dconf->polysign);
	free(dconf->polynums);
	free(dconf->polymap);

	for (i = 0; i < dconf->numbins; i++)
	{
		free(dconf->bins1[i].root);
		free(dconf->bins2[i].root);
		free(dconf->bins1[i].polynum);
		free(dconf->bins2[i].polynum);
	}

	free(dconf->bins1);
	free(dconf->bins2);

	free(dconf->ss_set1.root);
	free(dconf->ss_set1.polynum);
	free(dconf->ss_set2.root);
	free(dconf->ss_set2.polynum);

	return;
}

void ss_search_setup(static_conf_t* sconf, dynamic_conf_t* dconf)
{
	siqs_poly* poly = dconf->curr_poly;
	fb_list* fb = sconf->factor_base;
	uint32_t numblocks = sconf->num_blocks;
	uint32_t interval;
	int slicesz = sconf->factor_base->slice_size;

	numblocks = sconf->num_blocks;
	interval = numblocks << 15;

	int i, j, k, ii;
	int ss_num_poly_terms = poly->s / 2;
	ss_set_t* ss_set1 = &dconf->ss_set1;
	ss_set_t* ss_set2 = &dconf->ss_set2;

	mpz_init(dconf->polyb1);
	mpz_init(dconf->polyb2);

	ss_set1->root = (int*)xmalloc((2 << ss_num_poly_terms) * sizeof(int));
	ss_set1->polynum = (int*)xmalloc((2 << ss_num_poly_terms) * sizeof(int));
	ss_set1->size = ss_num_poly_terms;

	mpz_set(dconf->polyb1, dconf->Bl[0]);
	for (ii = 1; ii < ss_num_poly_terms; ii++) {
		mpz_add(dconf->polyb1, dconf->polyb1, dconf->Bl[ii]);
	}
	mpz_tdiv_q_2exp(dconf->polyb1, dconf->polyb1, 1);

	ss_num_poly_terms = (poly->s - 1) - ss_num_poly_terms;

	ss_set2->root = (int*)xmalloc((1 << ss_num_poly_terms) * sizeof(int));
	ss_set2->polynum = (int*)xmalloc((1 << ss_num_poly_terms) * sizeof(int));
	ss_set2->size = ss_num_poly_terms;

	//printf("ss_setup: %d,%d terms on L,R sides\n", ss_set1->size, ss_set2->size);

	// first poly b: sum of first n positive Bl
	mpz_set(dconf->polyb2, dconf->Bl[ii]);
	for (ii++; ii < poly->s; ii++) {
		mpz_add(dconf->polyb2, dconf->polyb2, dconf->Bl[ii]);
	}
	mpz_tdiv_q_2exp(dconf->polyb2, dconf->polyb2, 1);

	dconf->numbins = 2 * fb->list->prime[fb->B - 1] / (interval)+1;
	dconf->bindepth = 256;

	int numbins = dconf->numbins;
	int bindepth = dconf->bindepth;

	ss_set_t* bins1;
	ss_set_t* bins2;

	// init bins
	bins1 = (ss_set_t*)xmalloc(numbins * sizeof(ss_set_t));
	bins2 = (ss_set_t*)xmalloc(numbins * sizeof(ss_set_t));

	for (ii = 0; ii < numbins; ii++)
	{
		bins1[ii].root = (int*)xmalloc(bindepth * sizeof(int));
		bins2[ii].root = (int*)xmalloc(bindepth * sizeof(int));
		bins1[ii].polynum = (int*)xmalloc(bindepth * sizeof(int));
		bins2[ii].polynum = (int*)xmalloc(bindepth * sizeof(int));
		bins1[ii].alloc = bindepth;
		bins2[ii].alloc = bindepth;
		bins1[ii].size = 0;
		bins2[ii].size = 0;
	}

	dconf->bins1 = bins1;
	dconf->bins2 = bins2;

	// create a map from gray code enumeration order
	// to polynomial binary encoding.
	int polynum = 0;
	dconf->polymap = (int*)xmalloc((1 << (poly->s - 1)) * sizeof(int));
	dconf->polynums = (int*)xmalloc((1 << (poly->s - 1)) * sizeof(int));
	dconf->polyv = (int*)xmalloc((1 << (poly->s - 1)) * sizeof(int));
	dconf->polysign = (int*)xmalloc((1 << (poly->s - 1)) * sizeof(int));

	//polymap[0] = 1;
	dconf->polymap[1] = 0;
	dconf->polynums[0] = 0;

	for (ii = 1, polynum = 0; ii < (1 << (poly->s - 1)); ii++)
	{
		int v;
		int tmp;
		int sign;

		// next poly enumeration
		v = 1;
		j = ii;
		while ((j & 1) == 0)
			v++, j >>= 1;
		tmp = ii + (1 << v) - 1;
		tmp = (tmp >> v);
		dconf->polyv[ii] = v;
		if (tmp & 1)
			dconf->polysign[ii] = -1;
		else
			dconf->polysign[ii] = 1;

		// next polynum
		polynum ^= (1 << (v - 1));
		dconf->polynums[ii] = polynum;
		dconf->polymap[ii + 1] = polynum;

		//printf("%d <- %d (%d)\n", ii + 1, polynum, polynums[ii]);
	}

	int num_bpoly = 1 << (dconf->curr_poly->s - 1);

	if (num_bpoly > dconf->ss_slices_p[0].numbuckets)
	{
		printf("not enough buckets allocated (%d) for current number of b-polys (%d)\n",
			dconf->ss_slices_p[0].numbuckets, num_bpoly);
		exit(1);
	}

	for (i = 0; i < dconf->num_ss_slices; i++)
	{
		for (ii = 0; ii <= (2 * num_bpoly); ii++)
		{
			dconf->ss_slices_p[i].size[ii] = 0;
			//dconf->ss_slices_n[i].size[ii] = 0;
		}

		dconf->ss_slices_p[i].curr_poly_idx = 0;
		dconf->ss_slices_n[i].curr_poly_idx = 0;
		dconf->ss_slices_p[i].curr_poly_num = 0;
		dconf->ss_slices_n[i].curr_poly_num = 0;
	}

	return;
}

void ss_search_sort_set_1(static_conf_t* sconf, dynamic_conf_t* dconf)
{
	siqs_poly* poly = dconf->curr_poly;
	fb_list* fb = sconf->factor_base;
	uint32_t numblocks = sconf->num_blocks;
	uint32_t interval;
	int slicesz = sconf->factor_base->slice_size;

	numblocks = sconf->num_blocks;
	interval = numblocks << 15;

	ss_set_t* ss_set1 = &dconf->ss_set1;

	int* rootupdates = dconf->rootupdates;
	update_t update_data = dconf->update_data;
	uint32_t* modsqrt = sconf->modsqrt_array;

	int i, j, k, ii;

	struct timeval start, stop;
	double t_enum_roots = 0.0;
	double t_sort_roots = 0.0;

	dconf->bins1_mp = (ss_set_t**)xmalloc((fb->B - fb->ss_start_B) * sizeof(ss_set_t*));
	int size1 = ss_set1->size;
	int bindepth = dconf->bindepth;

	for (i = fb->ss_start_B; i < fb->B; i++)
	{
		int r1 = update_data.firstroots1[i], r2 = update_data.firstroots2[i];
		uint32_t bound = sconf->factor_base->B;
		int polynum = 0;
		int slice = (int)((i - sconf->factor_base->ss_start_B) / slicesz);
		int fboffset = dconf->ss_slices_p[slice].fboffset;
		int prime;
		int root1;
		int root2;
		uint8_t logp = fb->list->logprime[i];

		if (slice >= sconf->factor_base->num_ss_slices)
		{
			printf("error slice number %d too large, %d allocated, at fb index %d (prime %u)\n",
				slice, sconf->factor_base->num_ss_slices, i, prime);
			exit(0);
		}

		// create set1, an enumeration of (+/-t - b)(a)^-1 mod p
		// for b in the set [+/-B1 +/- B2 +/- ... Bs/2]
		int ii;
		int tmp;

#ifdef SS_TIMING
		gettimeofday(&start, NULL);
#endif

		prime = fb->list->prime[i];
		r1 = dconf->firstroot1a[i];
		r2 = dconf->firstroot1b[i];

		int* ptr;

		// enumerate set1 roots
		for (ii = 1, k = 0, polynum = 0; ii < (1 << size1); ii++, k += 2) {
			// these roots go into the set
			ss_set1->root[k + 0] = r1;
			ss_set1->root[k + 1] = r2;
			ss_set1->polynum[k + 0] = polynum;
			ss_set1->polynum[k + 1] = polynum;

			// next polynum
			polynum = dconf->polynums[ii];

			// next roots
			ptr = &rootupdates[(dconf->polyv[ii] - 1) * bound + i];
			if (dconf->polysign[ii] > 0)
			{
				r1 = (int)r1 - *ptr;
				r2 = (int)r2 - *ptr;
				r1 = (r1 < 0) ? r1 + prime : r1;
				r2 = (r2 < 0) ? r2 + prime : r2;
			}
			else
			{
				r1 = (int)r1 + *ptr;
				r2 = (int)r2 + *ptr;
				r1 = (r1 >= prime) ? r1 - prime : r1;
				r2 = (r2 >= prime) ? r2 - prime : r2;
			}
		}
		// these roots go into the set
		ss_set1->root[k + 0] = r1;
		ss_set1->root[k + 1] = r2;
		ss_set1->polynum[k + 0] = polynum;
		ss_set1->polynum[k + 1] = polynum;

#ifdef SS_TIMING
		gettimeofday(&stop, NULL);
		t_enum_roots += ytools_difftime(&start, &stop);

		gettimeofday(&start, NULL);
#endif

		// sort into a set of bins for this prime
		int numbins = prime / (6 * interval) + 1;
		int binsize = prime / numbins + 1;
		int binid = i - fb->ss_start_B;

		dconf->bins1_mp[binid] = (ss_set_t*)xmalloc(numbins * sizeof(ss_set_t));

		// initialize bin sizes
		for (ii = 0; ii < numbins; ii++)
		{
			dconf->bins1_mp[binid][ii].root = (int*)xmalloc(bindepth * sizeof(int));
			dconf->bins1_mp[binid][ii].polynum = (int*)xmalloc(bindepth * sizeof(int));
			dconf->bins1_mp[binid][ii].alloc = bindepth;
			dconf->bins1_mp[binid][ii].size = 0;
		}

		// sort roots into set 1
		for (ii = 0; ii < (2 << ss_set1->size); ii++)
		{
			int binnum = ss_set1->root[ii] / binsize;
			if (binnum < numbins)
			{
				int sz = dconf->bins1_mp[binid][binnum].size;
				dconf->bins1_mp[binid][binnum].root[sz] = ss_set1->root[ii];
				dconf->bins1_mp[binid][binnum].polynum[sz] = ss_set1->polynum[ii];
				dconf->bins1_mp[binid][binnum].size++;
				if (dconf->bins1_mp[binid][binnum].size >= bindepth)
				{
					printf("\nbin overflow\n\n");
					exit(1);
				}
			}
			else
			{
				printf("element %d of set 1, prime %u, root %d, invalid bin %d [of %d] with binsize %u\n",
					ii, prime, ss_set1->root[ii], binnum, numbins, binsize);
				exit(1);
			}
		}

#ifdef SS_TIMING
		gettimeofday(&stop, NULL);
		t_sort_roots += ytools_difftime(&start, &stop);
#endif
	}

#ifdef SS_TIMING
	printf("enumerating roots: %1.4f seconds\n", t_enum_roots);
	printf("sorting roots: %1.4f seconds\n", t_sort_roots);
#endif

	return;
}



void ss_search_poly_buckets(static_conf_t* sconf, dynamic_conf_t* dconf)
{
	// the subset-sum search algorithm using a bucket sort to
	// organize the sieve-hits per poly.  
	siqs_poly* poly = dconf->curr_poly;
	fb_list* fb = sconf->factor_base;
	uint32_t numblocks = sconf->num_blocks;
	uint32_t interval;
	int slicesz = sconf->factor_base->slice_size;

	numblocks = sconf->num_blocks;
	interval = numblocks << 15;

	int i, j, k, ii;
	
	ss_set_t* ss_set1 = &dconf->ss_set1;
	ss_set_t* ss_set2 = &dconf->ss_set2;
	ss_set_t* bins1 = dconf->bins1;
	ss_set_t* bins2 = dconf->bins2;

	double t_enum_roots;
	double t_sort_roots;
	double t_match_roots;
	double t_sort_buckets;
	struct timeval start, stop;

	int* rootupdates = dconf->rootupdates;
	update_t update_data = dconf->update_data;
	uint32_t* modsqrt = sconf->modsqrt_array;

	int maxbin1 = 0;
	int maxbin2 = 0;
	int nummatch = 0;
	int matchp1 = 0;
	int matchm1 = 0;

	double avg_size1 = 0.0;
	double avg_size2 = 0.0;
	double avg_size3 = 0.0;

	int totalbins1 = 0;
	int totalbins2 = 0;
	int totalbins3 = 0;

	int nump = 0;

	for (i = fb->ss_start_B; i < fb->B; i++)
	{
		int numB;
		int maxB = 1 << (dconf->curr_poly->s - 1);
		int r1 = update_data.firstroots1[i], r2 = update_data.firstroots2[i];
		uint32_t bound = sconf->factor_base->B;
		uint32_t med_B = sconf->factor_base->med_B;
		int polynum = 0;
		int slice = (int)((i - sconf->factor_base->ss_start_B) / slicesz);
		int fboffset = dconf->ss_slices_p[slice].fboffset;
		int prime;
		int root1;
		int root2;
		uint8_t logp = fb->list->logprime[i];

		if (slice >= sconf->factor_base->num_ss_slices)
		{
			printf("error slice number %d too large, %d allocated, at fb index %d (prime %u)\n",
				slice, sconf->factor_base->num_ss_slices, i, prime);
			exit(0);
		}

		nump++;

		// create set1, an enumeration of (+/-t - b)(a)^-1 mod p
		// for b in the set [+/-B1 +/- B2 +/- ... Bs/2]
		int ii, v, sign, n = poly->s / 2;
		int tmp;

#ifdef SS_TIMING
		gettimeofday(&start, NULL);
#endif

		prime = fb->list->prime[i];
		r1 = dconf->firstroot1a[i];
		r2 = dconf->firstroot1b[i];

		int* ptr;

		// enumerate set1 roots
		for (ii = 1, k = 0, polynum = 0; ii < (1 << n); ii++, k += 2) {
			// these roots go into the set
			ss_set1->root[k + 0] = r1;
			ss_set1->root[k + 1] = r2;
			ss_set1->polynum[k + 0] = polynum;
			ss_set1->polynum[k + 1] = polynum;

			// next polynum
			polynum = dconf->polynums[ii];
			sign = dconf->polysign[ii];
			v = dconf->polyv[ii];

			// next roots
			ptr = &rootupdates[(v - 1) * bound + i];
			if (sign > 0)
			{
				r1 = (int)r1 - *ptr;
				r2 = (int)r2 - *ptr;
				r1 = (r1 < 0) ? r1 + prime : r1;
				r2 = (r2 < 0) ? r2 + prime : r2;
			}
			else
			{
				r1 = (int)r1 + *ptr;
				r2 = (int)r2 + *ptr;
				r1 = (r1 >= prime) ? r1 - prime : r1;
				r2 = (r2 >= prime) ? r2 - prime : r2;
			}
		}
		// these roots go into the set
		ss_set1->root[k + 0] = r1;
		ss_set1->root[k + 1] = r2;
		ss_set1->polynum[k + 0] = polynum;
		ss_set1->polynum[k + 1] = polynum;

		int size1 = n;

		// create set2, an enumeration of (+/-t - b)(a)^-1 mod p
		// for b in the set [+/-Bs/2+1 +/- Bs/2+2 ... + Bs]
		n = (poly->s - 1) - n;

		r1 = dconf->firstroot2[i];

		uint32_t polymask = (1 << ss_set1->size) - 1;

		//if (ss_set1->size == ss_set2->size)
		//{
		//	printf("polys generated by set2 polynum %d:\n", 0);
		//	for (k = 0; k < (2 << ss_set1->size); k += 2)
		//	{
		//		printf("binary poly encoding: %d, masked: %04x, gray code enumeration: %d, masked: %04x\n",
		//			ss_set1->polynum[k] + 0, (ss_set1->polynum[k] + 0) & polymask,
		//			dconf->polymap[ss_set1->polynum[k] + 0],
		//			dconf->polymap[ss_set1->polynum[k] + 0] & polymask);
		//	}
		//}

		// enumerate set2 roots.
		// each one of these generates a set of 2^n1 polys where
		// n1 is the size of set1.  These will all be the next
		// 2^n1 polys needed in the gray code enumeration with
		// the exception of the powers of 2.  
		// 
		// Except it's not.  For example when the number of terms
		// is equal on the left and right side then the a block of 
		// binary encoded polynums doesn't always map to the same
		// block of gray-encoded polynums.  There is still a regular
		// pattern so this scheme could work, but since initial tests
		// indicate its still not any faster then it doesn't seem
		// worth it to work out a solution.
		// Although even if it isn't any faster, the drastically reduced
		// memory usage means we could use SSS for much larger numbers.
		// 
		// Assuming we generate
		// and store the powers of 2 somewhere, then each call here
		// can reuse a set of 2^n1 poly buckets for the next 2^n1
		// gray-code polys.
		for (ii = 1, polynum = 0; ii < (1 << n); ii++) {
			// these roots go into the set
			ss_set2->root[ii - 1] = r1;
			ss_set2->polynum[ii - 1] = polynum;

			polynum = dconf->polynums[ii] << size1;
			sign = dconf->polysign[ii];
			v = dconf->polyv[ii];

			// next roots
			ptr = &rootupdates[(size1 + v - 1) * bound + i];
			if (sign > 0)
			{
				r1 = (int)r1 - *ptr;
				r1 = (r1 < 0) ? r1 + prime : r1;
			}
			else
			{
				r1 = (int)r1 + *ptr;
				r1 = (r1 >= prime) ? r1 - prime : r1;
			}

			//if (ss_set1->size == ss_set2->size)
			//{
			//	printf("polys generated by set2 polynum %d:\n", ii);
			//	for (k = 0; k < (2 << ss_set1->size); k += 2)
			//	{
			//		printf("binary poly encoding: %d, masked: %04x, gray code enumeration: %d, masked: %04x\n",
			//			ss_set1->polynum[k] + polynum, (ss_set1->polynum[k] + polynum) & polymask,
			//			dconf->polymap[ss_set1->polynum[k] + polynum],
			//			dconf->polymap[ss_set1->polynum[k] + polynum] & polymask);
			//	}
			//}
		}

		//if (ss_set1->size == ss_set2->size)
		//{
		//	exit(1);
		//}

		// these roots go into the set
		ss_set2->root[ii - 1] = r1;
		ss_set2->polynum[ii - 1] = polynum;

#ifdef SS_TIMING
		gettimeofday(&stop, NULL);
		t_enum_roots += ytools_difftime(&start, &stop);

		gettimeofday(&start, NULL);
#endif

		// now sort the sets into a moderate number of bins over the range 0:p
		dconf->numbins = prime / (6 * interval) + 1;
		dconf->binsize = prime / dconf->numbins + 1;

		int numbins = dconf->numbins;
		int binsize = dconf->binsize;
		int bindepth = dconf->bindepth;

		// initialize bin sizes
		for (ii = 0; ii < numbins; ii++)
		{
			bins1[ii].size = 0;
			bins2[ii].size = 0;
		}

		// sort roots into set 1
		for (ii = 0; ii < (2 << ss_set1->size); ii++)
		{
			int binnum = ss_set1->root[ii] / binsize;
			if (binnum < numbins)
			{
				bins1[binnum].root[bins1[binnum].size] = ss_set1->root[ii];
				bins1[binnum].polynum[bins1[binnum].size] = ss_set1->polynum[ii];
				bins1[binnum].size++;
				if (bins1[binnum].size >= bindepth)
				{
					printf("\nbin overflow\n\n");
					exit(1);
				}
			}
			else
			{
				printf("element %d of set 1, root %d, invalid bin %d [of %d]\n",
					ii, ss_set1->root[ii], binnum, numbins);
			}
		}

		// sort set 2
		for (ii = 0; ii < (1 << ss_set2->size); ii++)
		{
			int binnum = ss_set2->root[ii] / binsize;
			if (binnum < numbins)
			{
				bins2[binnum].root[bins2[binnum].size] = ss_set2->root[ii];
				bins2[binnum].polynum[bins2[binnum].size] = ss_set2->polynum[ii];
				bins2[binnum].size++;
				if (bins2[binnum].size > bindepth)
				{
					printf("bin overflow\n");
					exit(1);
				}
			}
			else
			{
				printf("element %d of set 2, root %d, invalid bin %d [of %d]\n",
					ii, ss_set2->root[ii], binnum, numbins);
			}
		}


#ifdef SS_TIMING
		gettimeofday(&stop, NULL);
		t_sort_roots += ytools_difftime(&start, &stop);

		gettimeofday(&start, NULL);
#endif

		// commence matching
		uint32_t pid = (uint32_t)(i - fboffset) << 18;

		if ((i - fboffset) >= slicesz)
		{
			printf("pid %d too large for slice %d with fboffset %d\n",
				pid, slice, fboffset);
			exit(1);
		}

		for (ii = 0; ii < numbins; ii++)
		{
			avg_size1 += bins1[ii].size;
			avg_size3 += bins2[ii].size;
		}
		totalbins1 += numbins;

		__m512i vp = _mm512_set1_epi32(prime);
		__m512i vi = _mm512_set1_epi32(interval);
		__m512i vz = _mm512_setzero_epi32();
		__m512i vpid = _mm512_set1_epi32(pid);

		uint32_t bucketalloc = dconf->ss_slices_p[0].alloc;
		uint32_t* pslice_ptr = dconf->ss_slices_p[slice].elements;
		uint32_t* nslice_ptr = dconf->ss_slices_n[slice].elements;
		uint32_t* psize_ptr = dconf->ss_slices_p[slice].size;
		uint32_t* nsize_ptr = dconf->ss_slices_n[slice].size;

		// ideas to try:
		// * don't sort all of the polys at once.  Increment set2 so that we sort
		//   set1-sized groups of polynomials at a time.  every so often come back 
		//   here to get some more. (difficult because binary-encoded polynomials 
		//   produces in batches of set2 don't necessarily match up to batches of
		//   gray-code polynomials.  Does drastically reduce the memory requirements
		//   though, so more work to find a solution might be worth it.  Might need
		//   to force an odd number of terms in poly-a.)
		// * sort the bins by poly prior to matching?  bins are relatively sparse
		//   so sorting them is hopefully fast.  Bin2 holds the upper bits
		//   of the poly and bin1 hold the lower bits, so when we match an element
		//   from bin2 to many from bin1, we create matches for many poly buckets
		//   that are near each other.  Buckets are still fairly big though, so
		//   even consecutive polys are not super close.  Still, may be helpful.
		// * process a couple b-bins at a time, the independent operations could 
		//   hide some latency.

		for (ii = 0; ii < numbins; ii++)
		{
			int x = binsize * ii + binsize / 2;
			int px = prime - x;
			int b = px / binsize;

			for (k = 0; k < bins2[b].size; k++)
			{
				__m512i vb2root = _mm512_set1_epi32(bins2[b].root[k]);
				__m512i vb2poly = _mm512_set1_epi32(bins2[b].polynum[k]);
				int bin2root = bins2[b].root[k];

#if 1
				//if (j = 0) //
				//for (j = 0; j < bins1[ii].size; j += 16)
				j = 0;
				if (bins1[ii].size > 4)
				{
					__mmask16 loadmask;
					
					if ((bins1[ii].size - j) >= 16)
						loadmask = 0xffff;
					else
						loadmask = (1 << (bins1[ii].size - j)) - 1;

					__m512i vb1root = _mm512_mask_loadu_epi32(vz, loadmask,
						&bins1[ii].root[j]);

					__m512i vsum = _mm512_add_epi32(vb1root, vb2root);
					__mmask16 mpos = loadmask & _mm512_cmpge_epi32_mask(vsum, vp);
					__mmask16 mneg = loadmask & (~mpos);

#ifdef TRY_PN_BUCKET_COMBINE
					__m512i vdiffp = _mm512_mask_sub_epi32(vp, mpos, vsum, vp);
					vdiffp = _mm512_mask_sub_epi32(vdiffp, mneg, vp, vsum);

					mpos = _mm512_cmplt_epi32_mask(vdiffp, vi);

					vdiffp = _mm512_or_epi32(vdiffp, vpid);
					vdiffp = _mm512_mask_or_epi32(vdiffp, mneg, vdiffp, _mm512_set1_epi32(1 << 17));

					__m512i vb1poly = _mm512_mask_loadu_epi32(vz, loadmask,
						&bins1[ii].polynum[j]);

#ifdef TRY_GATHER_SCATTER
					vb1poly = _mm512_add_epi32(vb1poly, vb2poly);
					__m512i vbsz = _mm512_mask_i32gather_epi32(vbsz, mpos, vb1poly, psize_ptr, 4);
					__m512i vindex = _mm512_slli_epi32(vb1poly, 12);
					vindex = _mm512_add_epi32(vindex, vbsz);

					_mm512_mask_i32scatter_epi32(pslice_ptr, mpos, vindex, vdiffp, 4);
					vbsz = _mm512_add_epi32(vbsz, _mm512_set1_epi32(1));
					_mm512_mask_i32scatter_epi32(psize_ptr, mpos, vb1poly, vbsz, 4);

					nummatch += _mm_popcnt_u32(mpos);
#else
					if (mpos > 0)
					{
						uint32_t sum[16], poly[16];
					
						_mm512_storeu_epi32(sum, vdiffp);
						_mm512_storeu_epi32(poly, _mm512_add_epi32(vb1poly, vb2poly));
					
						while (mpos > 0)
						{
							int pos = _trail_zcnt(mpos);
							int idx = poly[pos];
					
							uint32_t bsz = psize_ptr[idx];
							pslice_ptr[idx * bucketalloc + bsz] = sum[pos];
							psize_ptr[idx]++;
					
							if (psize_ptr[idx] >= dconf->ss_slices_p[slice].alloc)
								printf("error bucket overflow: size of poly bucket %d = %d\n",
									idx, psize_ptr[idx]);
					
							mpos = _reset_lsb(mpos);
							nummatch++;
						}
					}
#endif

#else
					__m512i vdiffp = _mm512_mask_sub_epi32(vp, mpos, vsum, vp);
					__m512i vdiffn = _mm512_mask_sub_epi32(vp, mneg, vp, vsum);

					mpos = _mm512_cmplt_epi32_mask(vdiffp, vi);
					mneg = _mm512_cmplt_epi32_mask(vdiffn, vi);

					vdiffp = _mm512_or_epi32(vdiffp, vpid);
					vdiffn = _mm512_or_epi32(vdiffn, vpid);

					__m512i vb1poly = _mm512_mask_loadu_epi32(vz, loadmask,
						&bins1[ii].polynum[j]);

					if (mpos > 0)
					{
						uint32_t sum[16], poly[16];

						_mm512_mask_storeu_epi32(sum, mpos, vdiffp);
						_mm512_mask_storeu_epi32(poly, mpos, _mm512_add_epi32(vb1poly, vb2poly));

						while (mpos > 0)
						{
							int pos = _trail_zcnt(mpos);
							int idx = poly[pos];

							uint32_t bsz = psize_ptr[idx];
							pslice_ptr[idx * bucketalloc + bsz] = sum[pos];
							psize_ptr[idx]++;

							if (psize_ptr[idx] >= 16384)
								printf("error bucket overflow: size of poly bucket %d = %d\n",
									idx, psize_ptr[idx]);

							mpos = _reset_lsb(mpos);
							nummatch++;
						}
					}

					if (mneg > 0)
					{
						uint32_t sum[16], poly[16];
					
						_mm512_mask_storeu_epi32(sum, mneg, vdiffn);
						_mm512_mask_storeu_epi32(poly, mneg, _mm512_add_epi32(vb1poly, vb2poly));
					
						while (mneg > 0)
						{
							int pos = _trail_zcnt(mneg);
							int idx = poly[pos];
					
							uint32_t bsz = nsize_ptr[idx];
							nslice_ptr[idx * bucketalloc + bsz] = sum[pos];
							nsize_ptr[idx]++;
					
							mneg = _reset_lsb(mneg);
							nummatch++;
						}
					}

#endif

					j += 16;
				}

				for (; j < bins1[ii].size; j++)
				{
					int sum1 = bin2root + bins1[ii].root[j];
					int sign1 = (sum1 >= prime);
					sum1 = sign1 ? sum1 - prime : prime - sum1;
				
					if (sum1 < interval)
					{
						int polysum = bins1[ii].polynum[j] + bins2[b].polynum[k];
						int idx = polysum;
				
#ifdef TRY_PN_BUCKET_COMBINE
						if (sign1)
						{
							uint32_t bsz = psize_ptr[idx];
							pslice_ptr[idx * bucketalloc + bsz] = (pid | sum1);
							psize_ptr[idx]++;
						}
						else
						{
							sum1 |= (1 << 17);
							uint32_t bsz = psize_ptr[idx];
							pslice_ptr[idx * bucketalloc + bsz] = (pid | sum1);
							psize_ptr[idx]++;
						}
#else
						if (sign1)
						{
							uint32_t bsz = psize_ptr[idx];
							pslice_ptr[idx * bucketalloc + bsz] = (pid | sum1);
							psize_ptr[idx]++;
					}
						else
						{
							uint32_t bsz = nsize_ptr[idx];
							nslice_ptr[idx * bucketalloc + bsz] = (pid | sum1);
							nsize_ptr[idx]++;
						}
#endif

						if (psize_ptr[idx] >= dconf->ss_slices_p[slice].alloc)
							printf("error bucket overflow: size of poly bucket %d = %d\n",
								idx, psize_ptr[idx]);
				
						nummatch++;
					}
				
				}

#else

				for (j = 0; j < MIN(bins1[ii].size, bins1b[ii].size); j++)
				{
					uint32_t sum1 = bin2root + bins1[ii].root[j];
					uint32_t sum2 = bin2root + bins1b[ii].root[j];
					int sign1 = (sum1 >= prime);
					int sign2 = (sum2 >= prime);
					sum1 = sign1 ? sum1 - prime : prime - sum1;
					sum2 = sign2 ? sum2 - prime : prime - sum2;

					if (sum1 < interval)
					{
						int polysum = bins1[ii].polynum[j] + bins2[b].polynum[k];
						int idx = polysum;

						if (sign1)
						{
							uint32_t bsz = dconf->ss_slices_p[slice].buckets[idx].size;

							//printf("adding element (%d | %d) to poly-bucket %d of size %d (alloc %d) in slice %d\n",
							//	pid, sum1, idx, bsz, dconf->ss_slices_p[slice].buckets[idx].alloc, slice);
							//fflush(stdout);

							dconf->ss_slices_p[slice].buckets[idx].element[bsz] = (pid | sum1);
							dconf->ss_slices_p[slice].buckets[idx].size++;

							if (dconf->ss_slices_p[slice].buckets[idx].size >=
								dconf->ss_slices_p[slice].buckets[idx].alloc)
							{
								dconf->ss_slices_p[slice].buckets[idx].alloc *= 2;
								dconf->ss_slices_p[slice].buckets[idx].element = (uint32_t*)xrealloc(
									dconf->ss_slices_p[slice].buckets[idx].element,
									dconf->ss_slices_p[slice].buckets[idx].alloc *
									sizeof(uint32_t));
							}
						}
						else
						{
							uint32_t bsz = dconf->ss_slices_n[slice].buckets[idx].size;

							dconf->ss_slices_n[slice].buckets[idx].element[bsz] = (pid | sum1);
							dconf->ss_slices_n[slice].buckets[idx].size++;

							if (dconf->ss_slices_n[slice].buckets[idx].size >=
								dconf->ss_slices_n[slice].buckets[idx].alloc)
							{
								dconf->ss_slices_n[slice].buckets[idx].alloc *= 2;
								dconf->ss_slices_n[slice].buckets[idx].element = (uint32_t*)xrealloc(
									dconf->ss_slices_n[slice].buckets[idx].element,
									dconf->ss_slices_n[slice].buckets[idx].alloc *
									sizeof(uint32_t));
							}
						}

						nummatch++;
					}

					if (sum2 < interval)
					{
						int polysum = bins1b[ii].polynum[j] + bins2[b].polynum[k];
						int idx = polysum;

						if (sign2)
						{
							uint32_t bsz = dconf->ss_slices_p[slice].buckets[idx].size;

							dconf->ss_slices_p[slice].buckets[idx].element[bsz] = (pid | sum2);
							dconf->ss_slices_p[slice].buckets[idx].size++;

							if (dconf->ss_slices_p[slice].buckets[idx].size >=
								dconf->ss_slices_p[slice].buckets[idx].alloc)
							{
								dconf->ss_slices_p[slice].buckets[idx].alloc *= 2;
								dconf->ss_slices_p[slice].buckets[idx].element = (uint32_t*)xrealloc(
									dconf->ss_slices_p[slice].buckets[idx].element,
									dconf->ss_slices_p[slice].buckets[idx].alloc *
									sizeof(uint32_t));
							}
						}
						else
						{
							uint32_t bsz = dconf->ss_slices_n[slice].buckets[idx].size;

							dconf->ss_slices_n[slice].buckets[idx].element[bsz] = (pid | sum2);
							dconf->ss_slices_n[slice].buckets[idx].size++;

							if (dconf->ss_slices_n[slice].buckets[idx].size >=
								dconf->ss_slices_n[slice].buckets[idx].alloc)
							{
								dconf->ss_slices_n[slice].buckets[idx].alloc *= 2;
								dconf->ss_slices_n[slice].buckets[idx].element = (uint32_t*)xrealloc(
									dconf->ss_slices_n[slice].buckets[idx].element,
									dconf->ss_slices_n[slice].buckets[idx].alloc *
									sizeof(uint32_t));
							}
						}

						nummatch++;
					}
				}

				if (bins1[ii].size > bins1b[ii].size)
				{
					for (; j < bins1[ii].size; j++)
					{
						uint32_t sum1 = bin2root + bins1[ii].root[j];
						int sign1 = (sum1 >= prime);
						sum1 = sign1 ? sum1 - prime : prime - sum1;

						if (sum1 < interval)
						{
							int polysum = bins1[ii].polynum[j] + bins2[b].polynum[k];
							int idx = polysum;

							if (sign1)
							{
								uint32_t bsz = dconf->ss_slices_p[slice].buckets[idx].size;

								dconf->ss_slices_p[slice].buckets[idx].element[bsz] = (pid | sum1);
								dconf->ss_slices_p[slice].buckets[idx].size++;

								if (dconf->ss_slices_p[slice].buckets[idx].size >=
									dconf->ss_slices_p[slice].buckets[idx].alloc)
								{
									dconf->ss_slices_p[slice].buckets[idx].alloc *= 2;
									dconf->ss_slices_p[slice].buckets[idx].element = (uint32_t*)xrealloc(
										dconf->ss_slices_p[slice].buckets[idx].element,
										dconf->ss_slices_p[slice].buckets[idx].alloc *
										sizeof(uint32_t));
								}
							}
							else
							{
								uint32_t bsz = dconf->ss_slices_n[slice].buckets[idx].size;

								dconf->ss_slices_n[slice].buckets[idx].element[bsz] = (pid | sum1);
								dconf->ss_slices_n[slice].buckets[idx].size++;

								if (dconf->ss_slices_n[slice].buckets[idx].size >=
									dconf->ss_slices_n[slice].buckets[idx].alloc)
								{
									dconf->ss_slices_n[slice].buckets[idx].alloc *= 2;
									dconf->ss_slices_n[slice].buckets[idx].element = (uint32_t*)xrealloc(
										dconf->ss_slices_n[slice].buckets[idx].element,
										dconf->ss_slices_n[slice].buckets[idx].alloc *
										sizeof(uint32_t));
								}
							}

							nummatch++;
						}

					}
				}
				else if (bins1[ii].size < bins1b[ii].size)
				{
					for (; j < bins1b[ii].size; j++)
					{
						uint32_t sum1 = bin2root + bins1b[ii].root[j];
						int sign1 = (sum1 >= prime);
						sum1 = sign1 ? sum1 - prime : prime - sum1;

						if (sum1 < interval)
						{
							int polysum = bins1b[ii].polynum[j] + bins2[b].polynum[k];
							int idx = polysum;

							if (sign1)
							{
								uint32_t bsz = dconf->ss_slices_p[slice].buckets[idx].size;

								dconf->ss_slices_p[slice].buckets[idx].element[bsz] = (pid | sum1);
								dconf->ss_slices_p[slice].buckets[idx].size++;

								if (dconf->ss_slices_p[slice].buckets[idx].size >=
									dconf->ss_slices_p[slice].buckets[idx].alloc)
								{
									dconf->ss_slices_p[slice].buckets[idx].alloc *= 2;
									dconf->ss_slices_p[slice].buckets[idx].element = (uint32_t*)xrealloc(
										dconf->ss_slices_p[slice].buckets[idx].element,
										dconf->ss_slices_p[slice].buckets[idx].alloc *
										sizeof(uint32_t));
								}
							}
							else
							{
								uint32_t bsz = dconf->ss_slices_n[slice].buckets[idx].size;

								dconf->ss_slices_n[slice].buckets[idx].element[bsz] = (pid | sum1);
								dconf->ss_slices_n[slice].buckets[idx].size++;

								if (dconf->ss_slices_n[slice].buckets[idx].size >=
									dconf->ss_slices_n[slice].buckets[idx].alloc)
								{
									dconf->ss_slices_n[slice].buckets[idx].alloc *= 2;
									dconf->ss_slices_n[slice].buckets[idx].element = (uint32_t*)xrealloc(
										dconf->ss_slices_n[slice].buckets[idx].element,
										dconf->ss_slices_n[slice].buckets[idx].alloc *
										sizeof(uint32_t));
								}
							}

							nummatch++;
						}

					}
				}
#endif

			}

#if 1

			if ((b + 1) < numbins)
			{
				for (k = 0; k < bins2[b + 1].size; k++)
				{
					__m512i vb2root = _mm512_set1_epi32(bins2[b + 1].root[k]);
					__m512i vb2poly = _mm512_set1_epi32(bins2[b + 1].polynum[k]);
					int bin2root = bins2[b + 1].root[k];

					//if (j = 0) //
					//for (j = 0; j < bins1[ii].size; j += 16)
					j = 0;
					if (bins1[ii].size > 4)
					{
						__mmask16 loadmask;

						if ((bins1[ii].size - j) >= 16)
							loadmask = 0xffff;
						else
							loadmask = (1 << (bins1[ii].size - j)) - 1;

						__m512i vb1root = _mm512_mask_loadu_epi32(vz, loadmask,
							&bins1[ii].root[j]);

						__m512i vsum = _mm512_add_epi32(vb1root, vb2root);
						__mmask16 mpos = loadmask & _mm512_cmpge_epi32_mask(vsum, vp);
						__mmask16 mneg = loadmask & (~mpos);

#ifdef TRY_PN_BUCKET_COMBINE
						__m512i vdiffp = _mm512_mask_sub_epi32(vp, mpos, vsum, vp);
						vdiffp = _mm512_mask_sub_epi32(vdiffp, mneg, vp, vsum);

						mpos = _mm512_cmplt_epi32_mask(vdiffp, vi);

						vdiffp = _mm512_or_epi32(vdiffp, vpid);
						vdiffp = _mm512_mask_or_epi32(vdiffp, mneg, vdiffp, _mm512_set1_epi32(1 << 17));

						__m512i vb1poly = _mm512_mask_loadu_epi32(vz, loadmask,
							&bins1[ii].polynum[j]);

#ifdef TRY_GATHER_SCATTER
						vb1poly = _mm512_add_epi32(vb1poly, vb2poly);
						__m512i vbsz = _mm512_mask_i32gather_epi32(vbsz, mpos, vb1poly, psize_ptr, 4);
						__m512i vindex = _mm512_slli_epi32(vb1poly, 12);
						vindex = _mm512_add_epi32(vindex, vbsz);

						_mm512_mask_i32scatter_epi32(pslice_ptr, mpos, vindex, vdiffp, 4);
						vbsz = _mm512_add_epi32(vbsz, _mm512_set1_epi32(1));
						_mm512_mask_i32scatter_epi32(psize_ptr, mpos, vb1poly, vbsz, 4);

						matchp1 += _mm_popcnt_u32(mpos);

#else
						if (mpos > 0)
						{
							uint32_t sum[16], poly[16];
						
							_mm512_storeu_epi32(sum, vdiffp);
							_mm512_storeu_epi32(poly, _mm512_add_epi32(vb1poly, vb2poly));
						
							while (mpos > 0)
							{
								int pos = _trail_zcnt(mpos);
								int idx = poly[pos];
						
								uint32_t bsz = psize_ptr[idx];
								pslice_ptr[idx * bucketalloc + bsz] = sum[pos];
								psize_ptr[idx]++;
						
								if (psize_ptr[idx] >= dconf->ss_slices_p[slice].alloc)
									printf("error bucket overflow: size of poly bucket %d = %d\n",
										idx, psize_ptr[idx]);
						
								mpos = _reset_lsb(mpos);
								matchp1++;
							}
						}
#endif

#else
						__m512i vdiffp = _mm512_mask_sub_epi32(vp, mpos, vsum, vp);
						__m512i vdiffn = _mm512_mask_sub_epi32(vp, mneg, vp, vsum);

						mpos = _mm512_cmplt_epi32_mask(vdiffp, vi);
						mneg = _mm512_cmplt_epi32_mask(vdiffn, vi);

						vdiffp = _mm512_or_epi32(vdiffp, vpid);
						vdiffn = _mm512_or_epi32(vdiffn, vpid);

						__m512i vb1poly = _mm512_mask_loadu_epi32(vz, loadmask,
							&bins1[ii].polynum[j]);

						if (mpos > 0)
						{
							uint32_t sum[16], poly[16];

							_mm512_mask_storeu_epi32(sum, mpos, vdiffp);
							_mm512_mask_storeu_epi32(poly, mpos, _mm512_add_epi32(vb1poly, vb2poly));

							while (mpos > 0)
							{
								int pos = _trail_zcnt(mpos);
								int idx = poly[pos];

								uint32_t bsz = psize_ptr[idx];
								pslice_ptr[idx * bucketalloc + bsz] = sum[pos];
								psize_ptr[idx]++;

								if (psize_ptr[idx] >= 16384)
									printf("error bucket overflow: size of poly bucket %d = %d\n",
										idx, psize_ptr[idx]);

								mpos = _reset_lsb(mpos);
								matchp1++;
							}
						}

						if (mneg > 0)
						{
							uint32_t sum[16], poly[16];

							_mm512_mask_storeu_epi32(sum, mneg, vdiffn);
							_mm512_mask_storeu_epi32(poly, mneg, _mm512_add_epi32(vb1poly, vb2poly));

							while (mneg > 0)
							{
								int pos = _trail_zcnt(mneg);
								int idx = poly[pos];

								uint32_t bsz = nsize_ptr[idx];
								nslice_ptr[idx * bucketalloc + bsz] = sum[pos];
								nsize_ptr[idx]++;

								mneg = _reset_lsb(mneg);
								matchp1++;
							}
						}

#endif

						j += 16;
					}

					for (; j < bins1[ii].size; j++)
					{
						int sum1 = bin2root + bins1[ii].root[j];
						int sign1 = (sum1 >= prime);
						sum1 = sign1 ? sum1 - prime : prime - sum1;

						if (sum1 < interval)
						{
							int polysum = bins1[ii].polynum[j] + bins2[b + 1].polynum[k];
							int idx = polysum;

#ifdef TRY_PN_BUCKET_COMBINE
							if (sign1)
							{
								uint32_t bsz = psize_ptr[idx];
								pslice_ptr[idx * bucketalloc + bsz] = (pid | sum1);
								psize_ptr[idx]++;
							}
							else
							{
								sum1 |= (1 << 17);
								uint32_t bsz = psize_ptr[idx];
								pslice_ptr[idx * bucketalloc + bsz] = (pid | sum1);
								psize_ptr[idx]++;
							}
#else
							if (sign1)
							{
								uint32_t bsz = psize_ptr[idx];
								pslice_ptr[idx * bucketalloc + bsz] = (pid | sum1);
								psize_ptr[idx]++;
							}
							else
							{
								uint32_t bsz = nsize_ptr[idx];
								nslice_ptr[idx * bucketalloc + bsz] = (pid | sum1);
								nsize_ptr[idx]++;
							}
#endif

							if (psize_ptr[idx] >= dconf->ss_slices_p[slice].alloc)
								printf("error bucket overflow: size of poly bucket %d = %d\n",
									idx, psize_ptr[idx]);

							matchp1++;
						}

					}
				}
			}

			if ((b - 1) >= 0)
			{
				for (k = 0; k < bins2[b - 1].size; k++)
				{
					__m512i vb2root = _mm512_set1_epi32(bins2[b - 1].root[k]);
					__m512i vb2poly = _mm512_set1_epi32(bins2[b - 1].polynum[k]);
					int bin2root = bins2[b - 1].root[k];

					//if (j = 0) //
					//for (j = 0; j < bins1[ii].size; j += 16)
					j = 0;
					if (bins1[ii].size > 4)
					{
						__mmask16 loadmask;

						if ((bins1[ii].size - j) >= 16)
							loadmask = 0xffff;
						else
							loadmask = (1 << (bins1[ii].size - j)) - 1;

						__m512i vb1root = _mm512_mask_loadu_epi32(vz, loadmask,
							&bins1[ii].root[j]);

						__m512i vsum = _mm512_add_epi32(vb1root, vb2root);
						__mmask16 mpos = loadmask & _mm512_cmpge_epi32_mask(vsum, vp);
						__mmask16 mneg = loadmask & (~mpos);

#ifdef TRY_PN_BUCKET_COMBINE
						__m512i vdiffp = _mm512_mask_sub_epi32(vp, mpos, vsum, vp);
						vdiffp = _mm512_mask_sub_epi32(vdiffp, mneg, vp, vsum);

						mpos = _mm512_cmplt_epi32_mask(vdiffp, vi);

						vdiffp = _mm512_or_epi32(vdiffp, vpid);
						vdiffp = _mm512_mask_or_epi32(vdiffp, mneg, vdiffp, _mm512_set1_epi32(1 << 17));

						__m512i vb1poly = _mm512_mask_loadu_epi32(vz, loadmask,
							&bins1[ii].polynum[j]);

#ifdef TRY_GATHER_SCATTER
						vb1poly = _mm512_add_epi32(vb1poly, vb2poly);
						__m512i vbsz = _mm512_mask_i32gather_epi32(vbsz, mpos, vb1poly, psize_ptr, 4);
						__m512i vindex = _mm512_slli_epi32(vb1poly, 12);
						vindex = _mm512_add_epi32(vindex, vbsz);

						_mm512_mask_i32scatter_epi32(pslice_ptr, mpos, vindex, vdiffp, 4);
						vbsz = _mm512_add_epi32(vbsz, _mm512_set1_epi32(1));
						_mm512_mask_i32scatter_epi32(psize_ptr, mpos, vb1poly, vbsz, 4);

						matchm1 += _mm_popcnt_u32(mpos);
#else

						if (mpos > 0)
						{
							uint32_t sum[16], poly[16];
						
							_mm512_mask_storeu_epi32(sum, mpos, vdiffp);
							_mm512_mask_storeu_epi32(poly, mpos, _mm512_add_epi32(vb1poly, vb2poly));
						
							while (mpos > 0)
							{
								int pos = _trail_zcnt(mpos);
								int idx = poly[pos];
						
								uint32_t bsz = psize_ptr[idx];
								pslice_ptr[idx * bucketalloc + bsz] = sum[pos];
								psize_ptr[idx]++;
						
								if (psize_ptr[idx] >= dconf->ss_slices_p[slice].alloc)
									printf("error bucket overflow: size of poly bucket %d = %d\n",
										idx, psize_ptr[idx]);
						
								mpos = _reset_lsb(mpos);
								matchm1++;
							}
						}
#endif

#else
						__m512i vdiffp = _mm512_mask_sub_epi32(vp, mpos, vsum, vp);
						__m512i vdiffn = _mm512_mask_sub_epi32(vp, mneg, vp, vsum);

						mpos = _mm512_cmplt_epi32_mask(vdiffp, vi);
						mneg = _mm512_cmplt_epi32_mask(vdiffn, vi);

						vdiffp = _mm512_or_epi32(vdiffp, vpid);
						vdiffn = _mm512_or_epi32(vdiffn, vpid);

						__m512i vb1poly = _mm512_mask_loadu_epi32(vz, loadmask,
							&bins1[ii].polynum[j]);

						if (mpos > 0)
						{
							uint32_t sum[16], poly[16];

							_mm512_mask_storeu_epi32(sum, mpos, vdiffp);
							_mm512_mask_storeu_epi32(poly, mpos, _mm512_add_epi32(vb1poly, vb2poly));

							while (mpos > 0)
							{
								int pos = _trail_zcnt(mpos);
								int idx = poly[pos];

								uint32_t bsz = psize_ptr[idx];
								pslice_ptr[idx * bucketalloc + bsz] = sum[pos];
								psize_ptr[idx]++;

								if (psize_ptr[idx] >= 16384)
									printf("error bucket overflow: size of poly bucket %d = %d\n",
										idx, psize_ptr[idx]);

								mpos = _reset_lsb(mpos);
								matchm1++;
							}
						}

						if (mneg > 0)
						{
							uint32_t sum[16], poly[16];

							_mm512_mask_storeu_epi32(sum, mneg, vdiffn);
							_mm512_mask_storeu_epi32(poly, mneg, _mm512_add_epi32(vb1poly, vb2poly));

							while (mneg > 0)
							{
								int pos = _trail_zcnt(mneg);
								int idx = poly[pos];

								uint32_t bsz = nsize_ptr[idx];
								nslice_ptr[idx * bucketalloc + bsz] = sum[pos];
								nsize_ptr[idx]++;

								mneg = _reset_lsb(mneg);
								matchm1++;
							}
						}

#endif

						j += 16;
					}

					for (; j < bins1[ii].size; j++)
					{
						int sum1 = bin2root + bins1[ii].root[j];
						int sign1 = (sum1 >= prime);
						sum1 = sign1 ? sum1 - prime : prime - sum1;

						if (sum1 < interval)
						{
							int polysum = bins1[ii].polynum[j] + bins2[b - 1].polynum[k];
							int idx = polysum;

#ifdef TRY_PN_BUCKET_COMBINE
							if (sign1)
							{
								uint32_t bsz = psize_ptr[idx];
								pslice_ptr[idx * bucketalloc + bsz] = (pid | sum1);
								psize_ptr[idx]++;
							}
							else
							{
								sum1 |= (1 << 17);
								uint32_t bsz = psize_ptr[idx];
								pslice_ptr[idx * bucketalloc + bsz] = (pid | sum1);
								psize_ptr[idx]++;
							}
#else
							if (sign1)
							{
								uint32_t bsz = psize_ptr[idx];
								pslice_ptr[idx * bucketalloc + bsz] = (pid | sum1);
								psize_ptr[idx]++;
							}
							else
							{
								uint32_t bsz = nsize_ptr[idx];
								nslice_ptr[idx * bucketalloc + bsz] = (pid | sum1);
								nsize_ptr[idx]++;
							}
#endif

							if (psize_ptr[idx] >= dconf->ss_slices_p[slice].alloc)
								printf("error bucket overflow: size of poly bucket %d = %d\n",
									idx, psize_ptr[idx]);

							matchm1++;
						}

					}
				}
			}
#endif

		}

		//exit(1);


#ifdef SS_TIMING
		gettimeofday(&stop, NULL);
		t_match_roots += ytools_difftime(&start, &stop);
#endif

	}


#ifdef SS_TIMING
	printf("ran subset-sum on %d primes\n", nump);
	printf("found %d sieve hits matching bins\n", nummatch);
	printf("found %d sieve hits matching bins + 1\n", matchp1);
	printf("found %d sieve hits matching bins - 1\n", matchm1);
	printf("avg_size bins1 = %1.4f\n", avg_size1 / totalbins1);
	printf("avg_size bins2 = %1.4f\n", avg_size3 / totalbins1);
	printf("enumerating roots: %1.4f seconds\n", t_enum_roots);
	printf("sorting roots: %1.4f seconds\n", t_sort_roots);
	printf("matching roots: %1.4f seconds\n", t_match_roots);
#endif

	//exit(0);

	return;
}

void ss_search_poly_buckets_2(static_conf_t* sconf, dynamic_conf_t* dconf, int set2_poly_id)
{
	// the subset-sum search algorithm using a bucket sort to
	// organize the sieve-hits per poly.  
	siqs_poly* poly = dconf->curr_poly;
	fb_list* fb = sconf->factor_base;
	uint32_t numblocks = sconf->num_blocks;
	uint32_t interval;
	int slicesz = sconf->factor_base->slice_size;

	numblocks = sconf->num_blocks;
	interval = numblocks << 15;

	int i, j, k, ii;

	ss_set_t* ss_set1 = &dconf->ss_set1;
	ss_set_t* ss_set2 = &dconf->ss_set2;
	ss_set_t* bins1 = dconf->bins1;
	ss_set_t* bins2 = dconf->bins2;

	double t_enum_roots;
	double t_sort_roots;
	double t_match_roots;
	double t_sort_buckets;
	struct timeval start, stop;

	int* rootupdates = dconf->rootupdates;
	update_t update_data = dconf->update_data;
	uint32_t* modsqrt = sconf->modsqrt_array;

	int maxbin1 = 0;
	int maxbin2 = 0;
	int nummatch = 0;
	int matchp1 = 0;
	int matchm1 = 0;

	double avg_size1 = 0.0;
	double avg_size2 = 0.0;
	double avg_size3 = 0.0;

	int totalbins1 = 0;
	int totalbins2 = 0;
	int totalbins3 = 0;

	int nump = 0;
	int size1 = poly->s / 2;
	int size2 = (poly->s - 1) - size1;
	int set2_poly = dconf->polynums[set2_poly_id - 1] << size1;

	int poly2_sign = dconf->polysign[set2_poly_id];
	int poly2_v = dconf->polyv[set2_poly_id];

	if (set2_poly_id == (1 << size2))
	{
		poly2_sign = -1;
		poly2_v = -1;
	}
	else
	{
		poly2_sign = dconf->polysign[set2_poly_id];
		poly2_v = dconf->polyv[set2_poly_id];
	}

	for (i = fb->ss_start_B; i < fb->B; i++)
	{
		int numB;
		int maxB = 1 << (dconf->curr_poly->s - 1);
		int r1 = update_data.firstroots1[i], r2 = update_data.firstroots2[i];
		uint32_t bound = sconf->factor_base->B;
		uint32_t med_B = sconf->factor_base->med_B;
		int polynum = 0;
		int slice = (int)((i - sconf->factor_base->ss_start_B) / slicesz);
		int fboffset = dconf->ss_slices_p[slice].fboffset;
		int prime;
		int root1;
		int root2;
		uint8_t logp = fb->list->logprime[i];

		if (slice >= sconf->factor_base->num_ss_slices)
		{
			printf("error slice number %d too large, %d allocated, at fb index %d (prime %u)\n",
				slice, sconf->factor_base->num_ss_slices, i, prime);
			exit(0);
		}

		nump++;

		int ii;
		int tmp;

		prime = fb->list->prime[i];
		r1 = dconf->firstroot1a[i];
		r2 = dconf->firstroot1b[i];

		int* ptr;

		// create set2 instance, an enumeration of (+/-t - b)(a)^-1 mod p
		// for b in the set [+/-Bs/2+1 +/- Bs/2+2 ... + Bs].
		// each set2 instance generates a set of 2^n1 polys where
		// n1 is the size of set1.  These will all be the next
		// 2^n1 polys needed in the gray code enumeration with
		// the exception of the powers of 2.  Assuming we generate
		// and store the powers of 2 somewhere, then each call here
		// can reuse a set of 2^n1 poly buckets for the next 2^n1
		// gray-code polys.

		// these roots go into the set
		r1 = dconf->firstroot2[i];
		int set2_root = r1;

		// next roots for this prime
		if (poly2_v >= 0)
		{
			ptr = &rootupdates[(size1 + poly2_v - 1) * bound + i];
			if (poly2_sign > 0)
			{
				r1 = (int)r1 - *ptr;
				r1 = (r1 < 0) ? r1 + prime : r1;
			}
			else
			{
				r1 = (int)r1 + *ptr;
				r1 = (r1 >= prime) ? r1 - prime : r1;
			}

			dconf->firstroot2[i] = r1;
		}

		// now sort the sets into a moderate number of bins over the range 0:p
		dconf->numbins = prime / (6 * interval) + 1;
		dconf->binsize = prime / dconf->numbins + 1;

		int numbins = dconf->numbins;
		int binsize = dconf->binsize;
		int bindepth = dconf->bindepth;

		// look up set1, an enumeration of (+/-t - b)(a)^-1 mod p
		// for b in the set [+/-B1 +/- B2 +/- ... Bs/2]
		int binid = i - fb->ss_start_B;
		bins1 = dconf->bins1_mp[binid];

		int binnum2 = set2_root / binsize;

#ifdef SS_TIMING
		gettimeofday(&start, NULL);
#endif

		// commence matching
		uint32_t pid = (uint32_t)(i - fboffset) << 18;

		if ((i - fboffset) >= slicesz)
		{
			printf("pid %d too large for slice %d with fboffset %d\n",
				pid, slice, fboffset);
			exit(1);
		}

		for (ii = 0; ii < numbins; ii++)
		{
			avg_size1 += bins1[ii].size;
		}
		totalbins1 += numbins;

		__m512i vp = _mm512_set1_epi32(prime);
		__m512i vi = _mm512_set1_epi32(interval);
		__m512i vz = _mm512_setzero_epi32();
		__m512i vpid = _mm512_set1_epi32(pid);

		uint32_t bucketalloc = dconf->ss_slices_p[0].alloc;
		uint32_t* pslice_ptr = dconf->ss_slices_p[slice].elements;
		uint32_t* psize_ptr = dconf->ss_slices_p[slice].size;

		// reuse the set1-size number of poly bins for this set2-instance.
		for (ii = 0; ii < (1 << size1); ii++)
		{
			psize_ptr[ii] = 0;
		}

		dconf->ss_slices_p[slice].curr_poly_idx = set2_poly;
		dconf->ss_slices_p[slice].curr_poly_num = size1;

		for (ii = 0; ii < numbins; ii++)
		{
			int x = binsize * ii + binsize / 2;
			int px = prime - x;
			int b = px / binsize;

			if (b == binnum2)
			{
				__m512i vb2root = _mm512_set1_epi32(set2_root);
				__m512i vb2poly = _mm512_set1_epi32(set2_poly);


				//if (j = 0) //
				//for (j = 0; j < bins1[ii].size; j += 16)
				j = 0;
				if (bins1[ii].size > 4)
				{
					__mmask16 loadmask;

					if ((bins1[ii].size - j) >= 16)
						loadmask = 0xffff;
					else
						loadmask = (1 << (bins1[ii].size - j)) - 1;

					__m512i vb1root = _mm512_mask_loadu_epi32(vz, loadmask,
						&bins1[ii].root[j]);

					__m512i vsum = _mm512_add_epi32(vb1root, vb2root);
					__mmask16 mpos = loadmask & _mm512_cmpge_epi32_mask(vsum, vp);
					__mmask16 mneg = loadmask & (~mpos);

					__m512i vdiffp = _mm512_mask_sub_epi32(vp, mpos, vsum, vp);
					vdiffp = _mm512_mask_sub_epi32(vdiffp, mneg, vp, vsum);

					mpos = _mm512_cmplt_epi32_mask(vdiffp, vi);

					vdiffp = _mm512_or_epi32(vdiffp, vpid);
					vdiffp = _mm512_mask_or_epi32(vdiffp, mneg, vdiffp, _mm512_set1_epi32(1 << 17));

					__m512i vb1poly = _mm512_mask_loadu_epi32(vz, loadmask,
						&bins1[ii].polynum[j]);

#ifdef TRY_GATHER_SCATTER
					vb1poly = _mm512_add_epi32(vb1poly, vb2poly);
					__m512i vbsz = _mm512_mask_i32gather_epi32(vbsz, mpos, vb1poly, psize_ptr, 4);
					__m512i vindex = _mm512_slli_epi32(vb1poly, 14);
					vindex = _mm512_add_epi32(vindex, vbsz);

					_mm512_mask_i32scatter_epi32(pslice_ptr, mpos, vindex, vdiffp, 4);
					vbsz = _mm512_add_epi32(vbsz, _mm512_set1_epi32(1));
					_mm512_mask_i32scatter_epi32(psize_ptr, mpos, vb1poly, vbsz, 4);

					nummatch += _mm_popcnt_u32(mpos);
#else
					if (mpos > 0)
					{
						uint32_t sum[16], poly[16];

						_mm512_storeu_epi32(sum, vdiffp);
						_mm512_storeu_epi32(poly, vb1poly); // _mm512_add_epi32(vb1poly, vb2poly));

						while (mpos > 0)
						{
							int pos = _trail_zcnt(mpos);
							int idx = poly[pos];

							uint32_t bsz = psize_ptr[idx];
							pslice_ptr[idx * bucketalloc + bsz] = sum[pos];
							psize_ptr[idx]++;

							if (psize_ptr[idx] >= 16384)
							{
								printf("error bucket overflow: size of poly bucket %d = %d\n",
									idx, psize_ptr[idx]);
								exit(1);
							}

							mpos = _reset_lsb(mpos);
							nummatch++;
						}
					}
#endif

					j += 16;
				}

				for (; j < bins1[ii].size; j++)
				{
					int sum1 = set2_root + bins1[ii].root[j];
					int sign1 = (sum1 >= prime);
					sum1 = sign1 ? sum1 - prime : prime - sum1;

					if (sum1 < interval)
					{
						int polysum = bins1[ii].polynum[j]; // +set2_poly;
						int idx = polysum;

						if (sign1)
						{
							uint32_t bsz = psize_ptr[idx];
							pslice_ptr[idx * bucketalloc + bsz] = (pid | sum1);
							psize_ptr[idx]++;
						}
						else
						{
							sum1 |= (1 << 17);
							uint32_t bsz = psize_ptr[idx];
							pslice_ptr[idx * bucketalloc + bsz] = (pid | sum1);
							psize_ptr[idx]++;
						}

						if (psize_ptr[idx] >= 16384)
						{
							printf("error bucket overflow: size of poly bucket %d = %d\n",
								idx, psize_ptr[idx]);
							exit(1);
						}

						nummatch++;
					}

				}

			}
		}

#ifdef SS_TIMING
		gettimeofday(&stop, NULL);
		t_match_roots += ytools_difftime(&start, &stop);
#endif

	}


#ifdef SS_TIMING
	//printf("ran subset-sum on %d primes\n", nump);
	//printf("found %d sieve hits in matching bins\n", nummatch);
	//printf("found %d sieve hits matching bins + 1\n", matchp1);
	//printf("found %d sieve hits matching bins - 1\n", matchm1);
	//printf("avg_size bins1 = %1.4f\n", avg_size1 / totalbins1);
	//printf("avg_size bins2 = %1.4f\n", avg_size3 / totalbins1);
	//printf("enumerating roots: %1.4f seconds\n", t_enum_roots);
	//printf("sorting roots: %1.4f seconds\n", t_sort_roots);
	//printf("matching roots: %1.4f seconds\n", t_match_roots);
#endif

	//exit(0);

	return;
}

#endif

#if defined( USE_DIRECT_SIEVE_SS )
void ss_search_clear(static_conf_t* sconf, dynamic_conf_t* dconf)
{
	int i;

	mpz_clear(dconf->polyb1);
	mpz_clear(dconf->polyb2);
	free(dconf->polyv);
	free(dconf->polysign);
	free(dconf->polynums);
	free(dconf->polymap);

	for (i = 0; i < dconf->numbins; i++)
	{
		free(dconf->bins1[i].root);
		free(dconf->bins2[i].root);
		free(dconf->bins1[i].polynum);
		free(dconf->bins2[i].polynum);
	}

	free(dconf->bins1);
	free(dconf->bins2);

	free(dconf->ss_set1.root);
	free(dconf->ss_set1.polynum);
	free(dconf->ss_set2.root);
	free(dconf->ss_set2.polynum);

	return;
}

void ss_search_setup(static_conf_t* sconf, dynamic_conf_t* dconf)
{
	siqs_poly* poly = dconf->curr_poly;
	fb_list* fb = sconf->factor_base;
	uint32_t numblocks = sconf->num_blocks;
	uint32_t interval;
	int slicesz = sconf->factor_base->slice_size;

	interval = dconf->ss_sieve_sz;

	int i, j, k, ii;
	int ss_num_poly_terms = poly->s / 2;
	ss_set_t* ss_set1 = &dconf->ss_set1;
	ss_set_t* ss_set2 = &dconf->ss_set2;

	mpz_init(dconf->polyb1);
	mpz_init(dconf->polyb2);

	ss_set1->root = (int*)xmalloc((2 << ss_num_poly_terms) * sizeof(int));
	ss_set1->polynum = (int*)xmalloc((2 << ss_num_poly_terms) * sizeof(int));
	ss_set1->size = ss_num_poly_terms;

	mpz_set(dconf->polyb1, dconf->Bl[0]);
	for (ii = 1; ii < ss_num_poly_terms; ii++) {
		mpz_add(dconf->polyb1, dconf->polyb1, dconf->Bl[ii]);
	}
	mpz_tdiv_q_2exp(dconf->polyb1, dconf->polyb1, 1);

	ss_num_poly_terms = (poly->s - 1) - ss_num_poly_terms;

	ss_set2->root = (int*)xmalloc((1 << ss_num_poly_terms) * sizeof(int));
	ss_set2->polynum = (int*)xmalloc((1 << ss_num_poly_terms) * sizeof(int));
	ss_set2->size = ss_num_poly_terms;

	//printf("ss_setup: %d,%d terms on L,R sides\n", ss_set1->size, ss_set2->size);

	// first poly b: sum of first n positive Bl
	mpz_set(dconf->polyb2, dconf->Bl[ii]);
	for (ii++; ii < poly->s; ii++) {
		mpz_add(dconf->polyb2, dconf->polyb2, dconf->Bl[ii]);
	}
	mpz_tdiv_q_2exp(dconf->polyb2, dconf->polyb2, 1);

	dconf->numbins = 2 * fb->list->prime[fb->B - 1] / (interval)+1;
	dconf->bindepth = 2048;

	int numbins = dconf->numbins;
	int bindepth = dconf->bindepth;

	ss_set_t* bins1;
	ss_set_t* bins2;

	// init bins
	bins1 = (ss_set_t*)xmalloc(numbins * sizeof(ss_set_t));
	bins2 = (ss_set_t*)xmalloc(numbins * sizeof(ss_set_t));

	for (ii = 0; ii < numbins; ii++)
	{
		bins1[ii].root = (int*)xmalloc(bindepth * sizeof(int));
		bins2[ii].root = (int*)xmalloc(bindepth * sizeof(int));
		bins1[ii].polynum = (int*)xmalloc(bindepth * sizeof(int));
		bins2[ii].polynum = (int*)xmalloc(bindepth * sizeof(int));
		bins1[ii].alloc = bindepth;
		bins2[ii].alloc = bindepth;
		bins1[ii].size = 0;
		bins2[ii].size = 0;
	}

	dconf->bins1 = bins1;
	dconf->bins2 = bins2;

	// create a map from gray code enumeration order
	// to polynomial binary encoding.
	int polynum = 0;
	dconf->polymap = (int*)xmalloc((2 << (poly->s - 1))  * sizeof(int));
	dconf->polynums = (int*)xmalloc((2 << (poly->s - 1)) * sizeof(int));
	dconf->polyv = (int*)xmalloc((2 << (poly->s - 1)) * sizeof(int));
	dconf->polysign = (int*)xmalloc((2 << (poly->s - 1)) * sizeof(int));

	//polymap[0] = 1;
	dconf->polymap[1] = 0;
	dconf->polynums[0] = 0;

	//printf("binary %d <- gray %d\n", dconf->polymap[1], 1);

	for (ii = 1, polynum = 0; ii < (1 << (poly->s - 1)); ii++)
	{
		int v;
		int tmp;
		int sign;

		// next poly enumeration
		v = 1;
		j = ii;
		while ((j & 1) == 0)
			v++, j >>= 1;
		tmp = ii + (1 << v) - 1;
		tmp = (tmp >> v);
		dconf->polyv[ii] = v;
		if (tmp & 1)
			dconf->polysign[ii] = -1;
		else
			dconf->polysign[ii] = 1;

		// next polynum
		polynum ^= (1 << (v - 1));
		dconf->polynums[ii] = polynum;
		dconf->polymap[ii + 1] = polynum;

		//printf("binary %d <- gray %d\n", dconf->polymap[ii + 1], ii + 1);
	}
	//exit(1);

	return;
}

void ss_search_poly_buckets(static_conf_t* sconf, dynamic_conf_t* dconf)
{
	// the subset-sum search algorithm that directly  
	// populates a tiny sieve region.
	siqs_poly* poly = dconf->curr_poly;
	fb_list* fb = sconf->factor_base;
	uint32_t numblocks = sconf->num_blocks;
	uint32_t interval;
	int slicesz = sconf->factor_base->slice_size;

	interval = dconf->ss_sieve_sz;

	int i, j, k, ii;

	ss_set_t* ss_set1 = &dconf->ss_set1;
	ss_set_t* ss_set2 = &dconf->ss_set2;
	ss_set_t* bins1 = dconf->bins1;
	ss_set_t* bins2 = dconf->bins2;

	double t_enum_roots;
	double t_sort_roots;
	double t_match_roots;
	double t_sort_buckets;
	struct timeval start, stop;

	int* rootupdates = dconf->rootupdates;
	update_t update_data = dconf->update_data;
	uint32_t* modsqrt = sconf->modsqrt_array;

	int maxbin1 = 0;
	int maxbin2 = 0;
	int nummatch = 0;
	int matchp1 = 0;
	int matchm1 = 0;

	double avg_size1 = 0.0;
	double avg_size2 = 0.0;
	double avg_size3 = 0.0;

	int totalbins1 = 0;
	int totalbins2 = 0;
	int totalbins3 = 0;

	int nump = 0;

	int resieve = dconf->num_ss_slices;

	//for (i = fb->ss_start_B; i < fb->B; i++)
	for (i = fb->fb_15bit_B; i < fb->B; i++)
	{
		uint32_t bound = sconf->factor_base->B;
		int polynum = 0;
		int prime;
		int root1;
		int root2;
		int r1;
		int r2;
		uint8_t logp = fb->list->logprime[i];

		nump++;

		// create set1, an enumeration of (+/-t - b)(a)^-1 mod p
		// for b in the set [+/-B1 +/- B2 +/- ... Bs/2]
		int ii, v, sign, n = poly->s / 2;
		int tmp;

#ifdef SS_TIMING
		gettimeofday(&start, NULL);
#endif

		prime = fb->list->prime[i];
		r1 = dconf->firstroot1a[i];
		r2 = dconf->firstroot1b[i];

		int* ptr;

		//if ((i & 32767) == 0) printf("sss-index = %d\n", i);

		// enumerate set1 roots
		for (ii = 1, k = 0, polynum = 0; ii < (1 << n); ii++, k += 2) {
			// these roots go into the set
			ss_set1->root[k + 0] = r1;
			ss_set1->root[k + 1] = r2;
			ss_set1->polynum[k + 0] = polynum;
			ss_set1->polynum[k + 1] = polynum;

			// next polynum
			polynum = dconf->polynums[ii];
			sign = dconf->polysign[ii];
			v = dconf->polyv[ii];

			// next roots
			ptr = &rootupdates[(v - 1) * bound + i];
			if (sign > 0)
			{
				r1 = (int)r1 - *ptr;
				r2 = (int)r2 - *ptr;
				r1 = (r1 < 0) ? r1 + prime : r1;
				r2 = (r2 < 0) ? r2 + prime : r2;
			}
			else
			{
				r1 = (int)r1 + *ptr;
				r2 = (int)r2 + *ptr;
				r1 = (r1 >= prime) ? r1 - prime : r1;
				r2 = (r2 >= prime) ? r2 - prime : r2;
			}
		}
		// these roots go into the set
		ss_set1->root[k + 0] = r1;
		ss_set1->root[k + 1] = r2;
		ss_set1->polynum[k + 0] = polynum;
		ss_set1->polynum[k + 1] = polynum;

		int size1 = n;

		// create set2, an enumeration of (+/-t - b)(a)^-1 mod p
		// for b in the set [+/-Bs/2+1 +/- Bs/2+2 ... + Bs]
		n = (poly->s - 1) - n;

		r1 = dconf->firstroot2[i];

		for (ii = 1, polynum = 0; ii < (1 << n); ii++) {
			// these roots go into the set
			ss_set2->root[ii - 1] = r1;
			ss_set2->polynum[ii - 1] = polynum;

			polynum = dconf->polynums[ii] << size1;
			sign = dconf->polysign[ii];
			v = dconf->polyv[ii];

			// next roots
			ptr = &rootupdates[(size1 + v - 1) * bound + i];
			if (sign > 0)
			{
				r1 = (int)r1 - *ptr;
				r1 = (r1 < 0) ? r1 + prime : r1;
			}
			else
			{
				r1 = (int)r1 + *ptr;
				r1 = (r1 >= prime) ? r1 - prime : r1;
			}
		}

		// these roots go into the set
		ss_set2->root[ii - 1] = r1;
		ss_set2->polynum[ii - 1] = polynum;

#ifdef SS_TIMING
		gettimeofday(&stop, NULL);
		t_enum_roots += ytools_difftime(&start, &stop);

		gettimeofday(&start, NULL);
#endif

		// now sort the sets into a moderate number of bins over the range 0:p
		dconf->numbins = prime / (6 * interval) + 1;
		dconf->binsize = prime / dconf->numbins + 1;

		int numbins = dconf->numbins;
		int binsize = dconf->binsize;
		int bindepth = dconf->bindepth;

		// initialize bin sizes
		for (ii = 0; ii < numbins; ii++)
		{
			bins1[ii].size = 0;
			bins2[ii].size = 0;
		}

		// sort roots into set 1
		for (ii = 0; ii < (2 << ss_set1->size); ii++)
		{
			int binnum = ss_set1->root[ii] / binsize;
			if (binnum < numbins)
			{
				bins1[binnum].root[bins1[binnum].size] = ss_set1->root[ii];
				bins1[binnum].polynum[bins1[binnum].size] = ss_set1->polynum[ii];
				bins1[binnum].size++;
				if (bins1[binnum].size >= bindepth)
				{
					printf("\nbin overflow\n\n");
					exit(1);
				}
			}
			else
			{
				printf("element %d of set 1, root %d, invalid bin %d [of %d]\n",
					ii, ss_set1->root[ii], binnum, numbins);
			}
		}

		// sort set 2
		for (ii = 0; ii < (1 << ss_set2->size); ii++)
		{
			int binnum = ss_set2->root[ii] / binsize;
			if (binnum < numbins)
			{
				bins2[binnum].root[bins2[binnum].size] = ss_set2->root[ii];
				bins2[binnum].polynum[bins2[binnum].size] = ss_set2->polynum[ii];
				bins2[binnum].size++;
				if (bins2[binnum].size > bindepth)
				{
					printf("bin overflow\n");
					exit(1);
				}
			}
			else
			{
				printf("element %d of set 2, root %d, invalid bin %d [of %d]\n",
					ii, ss_set2->root[ii], binnum, numbins);
			}
		}


#ifdef SS_TIMING
		gettimeofday(&stop, NULL);
		t_sort_roots += ytools_difftime(&start, &stop);

		gettimeofday(&start, NULL);
#endif

		// commence matching
		for (ii = 0; ii < numbins; ii++)
		{
			avg_size1 += bins1[ii].size;
			avg_size3 += bins2[ii].size;
		}
		totalbins1 += numbins;

		__m512i vp = _mm512_set1_epi32(prime);
		__m512i vi = _mm512_set1_epi32(interval);
		__m512i vz = _mm512_setzero_epi32();

		int sieve_sz = dconf->ss_sieve_sz;

		for (ii = 0; ii < numbins; ii++)
		{
			int x = binsize * ii + binsize / 2;
			int px = prime - x;
			int b = px / binsize;

			for (k = 0; k < bins2[b].size; k++)
			{
				__m512i vb2root = _mm512_set1_epi32(bins2[b].root[k]);
				__m512i vb2poly = _mm512_set1_epi32(bins2[b].polynum[k]);
				int bin2root = bins2[b].root[k];

				//if (j = 0) //
				//for (j = 0; j < bins1[ii].size; j += 16)
				j = 0;
				if (0) //bins1[ii].size > 4)
				{
					__mmask16 loadmask;

					if ((bins1[ii].size - j) >= 16)
						loadmask = 0xffff;
					else
						loadmask = (1 << (bins1[ii].size - j)) - 1;

					__m512i vb1root = _mm512_mask_loadu_epi32(vz, loadmask,
						&bins1[ii].root[j]);

					__m512i vsum = _mm512_add_epi32(vb1root, vb2root);
					__mmask16 mpos = loadmask & _mm512_cmpge_epi32_mask(vsum, vp);
					__mmask16 mneg = loadmask & (~mpos);

					j += 16;
				}

				for (; j < bins1[ii].size; j++)
				{
					int sum1 = bin2root + bins1[ii].root[j];
					int sign1 = (sum1 >= prime);
					sum1 = sign1 ? sum1 - prime : prime - sum1;

					if (sum1 < interval)
					{
						int polysum = bins1[ii].polynum[j] + bins2[b].polynum[k];
						int idx = polysum;

						if (resieve)
						{
							uint16_t report_num;

							if (sign1)
							{
								report_num = dconf->ss_sieve_p[idx * sieve_sz + sum1];
							}
							else
							{
								report_num = dconf->ss_sieve_n[idx * sieve_sz + sum1];
							}

							if (report_num < 0xff)
							{
								//printf("prime %d found to hit resieve idx %d at poly %d "
								//	"loc %d, side %d, currently at smooth_num = %d\n",
								//	prime, report_num, idx, sum1, sign1,
								//	dconf->smooth_num[report_num]);
								//gmp_printf("Qstart = %Zd\n", dconf->Qvals[report_num]);

								int smooth_num = dconf->smooth_num[report_num];
								while (mpz_tdiv_ui(dconf->Qvals[report_num], prime) == 0)
								{
									dconf->fb_offsets[report_num][smooth_num++] = i;
									mpz_tdiv_q_ui(dconf->Qvals[report_num],
										dconf->Qvals[report_num], prime);
								}

								//gmp_printf("smoothnum = %d, Qend = %Zd\n", smooth_num,
								//	dconf->Qvals[report_num]);
								dconf->smooth_num[report_num] = smooth_num;
							}
						}
						else
						{
							if (sign1)
							{
								dconf->ss_sieve_p[idx * sieve_sz + sum1] -= logp;
							}
							else
							{
								dconf->ss_sieve_n[idx * sieve_sz + sum1] -= logp;
							}
						}

						nummatch++;
					}
				}
			}

#if 1

			if ((b + 1) < numbins)
			{
				for (k = 0; k < bins2[b + 1].size; k++)
				{
					__m512i vb2root = _mm512_set1_epi32(bins2[b + 1].root[k]);
					__m512i vb2poly = _mm512_set1_epi32(bins2[b + 1].polynum[k]);
					int bin2root = bins2[b + 1].root[k];

					//if (j = 0) //
					//for (j = 0; j < bins1[ii].size; j += 16)
					j = 0;
					if (0) //bins1[ii].size > 4)
					{
						__mmask16 loadmask;

						if ((bins1[ii].size - j) >= 16)
							loadmask = 0xffff;
						else
							loadmask = (1 << (bins1[ii].size - j)) - 1;

						__m512i vb1root = _mm512_mask_loadu_epi32(vz, loadmask,
							&bins1[ii].root[j]);

						__m512i vsum = _mm512_add_epi32(vb1root, vb2root);
						__mmask16 mpos = loadmask & _mm512_cmpge_epi32_mask(vsum, vp);
						__mmask16 mneg = loadmask & (~mpos);

						j += 16;
					}

					for (; j < bins1[ii].size; j++)
					{
						int sum1 = bin2root + bins1[ii].root[j];
						int sign1 = (sum1 >= prime);
						sum1 = sign1 ? sum1 - prime : prime - sum1;

						if (sum1 < interval)
						{
							int polysum = bins1[ii].polynum[j] + bins2[b + 1].polynum[k];
							int idx = polysum;

							if (resieve)
							{
								int report_num;

								if (sign1)
									report_num = dconf->ss_sieve_p[idx * sieve_sz + sum1];
								else
									report_num = dconf->ss_sieve_n[idx * sieve_sz + sum1];

								if (report_num < 0xff)
								{
									//printf("prime %d found to hit resieve idx %d at poly %d "
									//	"loc %d, currently at smooth_num = %d\n",
									//	prime, report_num, idx, sum1,
									//	dconf->smooth_num[report_num]);
									//gmp_printf("Qstart = %Zd\n", dconf->Qvals[report_num]);

									int smooth_num = dconf->smooth_num[report_num];
									while (mpz_tdiv_ui(dconf->Qvals[report_num], prime) == 0)
									{
										dconf->fb_offsets[report_num][smooth_num++] = i;
										mpz_tdiv_q_ui(dconf->Qvals[report_num],
											dconf->Qvals[report_num], prime);
									}

									//gmp_printf("smoothnum = %d, Qend = %Zd\n", smooth_num,
									//	dconf->Qvals[report_num]);
									dconf->smooth_num[report_num] = smooth_num;
								}
							}
							else
							{
								if (sign1)
									dconf->ss_sieve_p[idx * sieve_sz + sum1] -= logp;
								else
									dconf->ss_sieve_n[idx * sieve_sz + sum1] -= logp;
							}
							matchp1++;
						}
					}
				}
			}

			if ((b - 1) >= 0)
			{
				for (k = 0; k < bins2[b - 1].size; k++)
				{
					__m512i vb2root = _mm512_set1_epi32(bins2[b - 1].root[k]);
					__m512i vb2poly = _mm512_set1_epi32(bins2[b - 1].polynum[k]);
					int bin2root = bins2[b - 1].root[k];

					//if (j = 0) //
					//for (j = 0; j < bins1[ii].size; j += 16)
					j = 0;
					if (0) //bins1[ii].size > 4)
					{
						__mmask16 loadmask;

						if ((bins1[ii].size - j) >= 16)
							loadmask = 0xffff;
						else
							loadmask = (1 << (bins1[ii].size - j)) - 1;

						__m512i vb1root = _mm512_mask_loadu_epi32(vz, loadmask,
							&bins1[ii].root[j]);

						__m512i vsum = _mm512_add_epi32(vb1root, vb2root);
						__mmask16 mpos = loadmask & _mm512_cmpge_epi32_mask(vsum, vp);
						__mmask16 mneg = loadmask & (~mpos);

						j += 16;
					}

					for (; j < bins1[ii].size; j++)
					{
						int sum1 = bin2root + bins1[ii].root[j];
						int sign1 = (sum1 >= prime);
						sum1 = sign1 ? sum1 - prime : prime - sum1;

						if (sum1 < interval)
						{
							int polysum = bins1[ii].polynum[j] + bins2[b - 1].polynum[k];
							int idx = polysum;

							if (resieve)
							{
								int report_num;

								if (sign1)
									report_num = dconf->ss_sieve_p[idx * sieve_sz + sum1];
								else
									report_num = dconf->ss_sieve_n[idx * sieve_sz + sum1];

								if (report_num < 0xff)
								{
									//printf("prime %d found to hit resieve idx %d at poly %d "
									//	"loc %d, currently at smooth_num = %d\n",
									//	prime, report_num, idx, sum1,
									//	dconf->smooth_num[report_num]);
									//gmp_printf("Qstart = %Zd\n", dconf->Qvals[report_num]);

									int smooth_num = dconf->smooth_num[report_num];
									while (mpz_tdiv_ui(dconf->Qvals[report_num], prime) == 0)
									{
										dconf->fb_offsets[report_num][smooth_num++] = i;
										mpz_tdiv_q_ui(dconf->Qvals[report_num],
											dconf->Qvals[report_num], prime);
									}

									//gmp_printf("smoothnum = %d, Qend = %Zd\n", smooth_num,
									//	dconf->Qvals[report_num]);
									dconf->smooth_num[report_num] = smooth_num;
								}
							}
							else
							{
								if (sign1)
									dconf->ss_sieve_p[idx * sieve_sz + sum1] -= logp;
								else
									dconf->ss_sieve_n[idx * sieve_sz + sum1] -= logp;
							}
							matchm1++;
						}
					}
				}
			}
#endif

		}

		//exit(1);


#ifdef SS_TIMING
		gettimeofday(&stop, NULL);
		t_match_roots += ytools_difftime(&start, &stop);
#endif

		}


#ifdef SS_TIMING
	//printf("ran subset-sum on %d primes\n", nump);
	//printf("found %d sieve hits matching bins\n", nummatch);
	//printf("found %d sieve hits matching bins + 1\n", matchp1);
	//printf("found %d sieve hits matching bins - 1\n", matchm1);
	//printf("avg_size bins1 = %1.4f\n", avg_size1 / totalbins1);
	//printf("avg_size bins2 = %1.4f\n", avg_size3 / totalbins1);
	//printf("enumerating roots: %1.4f seconds\n", t_enum_roots);
	//printf("sorting roots: %1.4f seconds\n", t_sort_roots);
	//printf("matching roots: %1.4f seconds\n", t_match_roots);
#endif

	//exit(0);

	return;
}
#endif

void firstRoots_32k(static_conf_t *sconf, dynamic_conf_t *dconf)
{
	//the roots are computed using a and b as follows:
	//(+/-t - b)(a)^-1 mod p
	//where the t values are the roots to t^2 = N mod p, found by shanks_tonelli
	//when constructing the factor base.
	//assume b > t

	//unpack stuff from the job data structures
	siqs_poly *poly = dconf->curr_poly;
	fb_list *fb = sconf->factor_base;
	uint32_t start_prime = 2;
	int *rootupdates = dconf->rootupdates;
	update_t update_data = dconf->update_data;
	sieve_fb_compressed *fb_p = dconf->comp_sieve_p;
	sieve_fb_compressed *fb_n = dconf->comp_sieve_n;
	lp_bucket *lp_bucket_p = dconf->buckets;
	uint32_t *modsqrt = sconf->modsqrt_array;

	//locals
	uint32_t i, interval;
	uint8_t logp;
    int root1, root2, nroot1, nroot2, prime, amodp, bmodp, inv, x, bnum, j, numblocks;
	int s = poly->s;
	int bound_index = 0, k;
	uint32_t bound_val = fb->med_B;
	uint32_t *bptr, *sliceptr_p, *sliceptr_n;
    uint64_t* bptr64, * sliceptr64_p, * sliceptr64_n;
    uint8_t* slicelogp_ptr = NULL;
    uint32_t* slicebound_ptr = NULL;
	uint32_t *numptr_p, *numptr_n;
	int check_bound = BUCKET_ALLOC/2 - 1, room;
    FILE *out;
    uint32_t shift = 24;
    int u = 0;
	int using_ss_search = 0;

	if ((sconf->factor_base->ss_start_B > 0) &&
		(sconf->factor_base->ss_start_B < sconf->factor_base->B))
	{
		using_ss_search = 1;
	}

	numblocks = sconf->num_blocks;
	interval = numblocks << 15;

	if (lp_bucket_p->list != NULL)
	{
		lp_bucket_p->fb_bounds[0] = fb->med_B;

		sliceptr_p = lp_bucket_p->list;
		sliceptr_n = lp_bucket_p->list + (numblocks << BUCKET_BITS);

		numptr_p = lp_bucket_p->num;
		numptr_n = lp_bucket_p->num + numblocks;
		//reset lp_buckets
		for (i=0;i< (2*numblocks*lp_bucket_p->alloc_slices) ;i++)
			numptr_p[i] = 0;

		lp_bucket_p->num_slices = 0;

        slicelogp_ptr = lp_bucket_p->logp;
        slicebound_ptr = lp_bucket_p->fb_bounds;
	}
	else
	{
		sliceptr_p = NULL;
		sliceptr_n = NULL;
		numptr_p = NULL;
		numptr_n = NULL;
	}

	for (i = start_prime; i < sconf->sieve_small_fb_start; i++)
	{
		uint64_t q64, tmp, t2;

		prime = fb->tinylist->prime[i];
		root1 = modsqrt[i]; 
		root2 = prime - root1; 

		amodp = (int)mpz_tdiv_ui(poly->mpz_poly_a,prime);
		bmodp = (int)mpz_tdiv_ui(poly->mpz_poly_b,prime);

		//find a^-1 mod p = inv(a mod p) mod p
        if (sconf->knmod8 == 1)
        {
            inv = modinv_1(2 * amodp, prime);
        }
        else
        {
            inv = modinv_1(amodp, prime);
        }

		COMPUTE_FIRST_ROOTS
	
		// reuse integer inverse of prime that we've calculated for use
		// in trial division stage
		// inv * root1 % prime
		t2 = (uint64_t)inv * (uint64_t)root1;
		tmp = t2 + (uint64_t)fb->tinylist->correction[i];
		q64 = tmp * (uint64_t)fb->tinylist->small_inv[i];
		tmp = q64 >> 32; 
		root1 = t2 - tmp * prime;

		// inv * root2 % prime
		t2 = (uint64_t)inv * (uint64_t)root2;
		tmp = t2 + (uint64_t)fb->tinylist->correction[i];
		q64 = tmp * (uint64_t)fb->tinylist->small_inv[i];
		tmp = q64 >> 32; 
		root2 = t2 - tmp * prime;
	
		//we don't sieve these primes, so ordering doesn't matter
		update_data.firstroots1[i] = root1;
		update_data.firstroots2[i] = root2;

		fb_p->root1[i] = (uint16_t)root1;
		fb_p->root2[i] = (uint16_t)root2;
		fb_n->root1[i] = (uint16_t)(prime - root2);
		fb_n->root2[i] = (uint16_t)(prime - root1);
		//if we were sieving, this would double count the location on the 
		//positive side.  but since we're not, its easier to check for inclusion
		//on the progression if we reset the negative root to zero if it is == prime
		if (fb_n->root1[i] == prime)
			fb_n->root1[i] = 0;
		if (fb_n->root2[i] == prime)
			fb_n->root2[i] = 0;

		//for this factor base prime, compute the rootupdate value for all s
		//Bl values.  amodp holds a^-1 mod p
		//the rootupdate value is given by 2*Bj*amodp
		//Bl[j] now holds 2*Bl
		for (j=0;j<s;j++)
		{
			x = (int)mpz_tdiv_ui(dconf->Bl[j],prime);
			
			// x * inv % prime
			t2 = (uint64_t)inv * (uint64_t)x;
			tmp = t2 + (uint64_t)fb->tinylist->correction[i];
			q64 = tmp * (uint64_t)fb->tinylist->small_inv[i];
			tmp = q64 >> 32; 
			x = t2 - tmp * prime;

			rootupdates[(j)*fb->B+i] = x;
		}
	}

	for (i=sconf->sieve_small_fb_start;i<fb->fb_15bit_B;i++)
	{
		uint64_t tmp, t2;

		prime = fb->list->prime[i];
		root1 = modsqrt[i]; 
		root2 = prime - root1; 

		amodp = (int)mpz_tdiv_ui(poly->mpz_poly_a,prime);
		bmodp = (int)mpz_tdiv_ui(poly->mpz_poly_b,prime);

		//find a^-1 mod p = inv(a mod p) mod p
        if (sconf->knmod8 == 1)
        {
            inv = modinv_1(2 * amodp, prime);
        }
        else
        {
            inv = modinv_1(amodp, prime);
        }

		COMPUTE_FIRST_ROOTS

        // Barrett reduction for inv * root
        t2 = (uint64_t)inv * (uint64_t)root1;
        tmp = (t2 * fb->list->binv[i]) >> 32;
        t2 -= tmp * prime;
        root1 = (t2 >= prime) ? t2 - prime : t2;

        t2 = (uint64_t)inv * (uint64_t)root2;
        tmp = (t2 * fb->list->binv[i]) >> 32;
        t2 -= tmp * prime;
        root2 = (t2 >= prime) ? t2 - prime : t2;

		if (root2 < root1)
		{
			update_data.sm_firstroots1[i] = (uint16_t)root2;
			update_data.sm_firstroots2[i] = (uint16_t)root1;

			fb_p->root1[i] = (uint16_t)root2;
			fb_p->root2[i] = (uint16_t)root1;
			fb_n->root1[i] = (uint16_t)(prime - root1);
			fb_n->root2[i] = (uint16_t)(prime - root2);
		}
		else
		{
			update_data.sm_firstroots1[i] = (uint16_t)root1;
			update_data.sm_firstroots2[i] = (uint16_t)root2;

			fb_p->root1[i] = (uint16_t)root1;
			fb_p->root2[i] = (uint16_t)root2;
			fb_n->root1[i] = (uint16_t)(prime - root2);
			fb_n->root2[i] = (uint16_t)(prime - root1);
		}


#if defined( USE_SS_SEARCH ) && defined( USE_DIRECT_SIEVE_SS )

		if (using_ss_search)
		{
			// put these roots directly into the sieve region.


		}
#endif

		//for this factor base prime, compute the rootupdate value for all s
		//Bl values.  amodp holds a^-1 mod p
		//the rootupdate value is given by 2*Bj*amodp
		//Bl[j] now holds 2*Bl
		for (j=0;j<s;j++)
		{
			x = (int)mpz_tdiv_ui(dconf->Bl[j],prime);

			// x * inv % prime
            t2 = (uint64_t)inv * (uint64_t)x;
            tmp = (t2 * fb->list->binv[i]) >> 32;
            t2 -= tmp * prime;
            x = (t2 >= prime) ? t2 - prime : t2;

			rootupdates[(j)*fb->B+i] = x;
			dconf->sm_rootupdates[(j)*fb->med_B+i] = (uint16_t)x;
		}

#if defined( USE_SS_SEARCH ) && defined( USE_DIRECT_SIEVE_SS )

		if (using_ss_search)
		{
			// enumerate the rest of the polys for this prime
			// and log any sieve hits.


		}
#endif
	}

	for (i=fb->fb_15bit_B;i<fb->med_B;i++)
	{
		uint64_t tmp, t2;

		prime = fb->list->prime[i];
		root1 = modsqrt[i]; 
		root2 = prime - root1; 

		amodp = (int)mpz_tdiv_ui(poly->mpz_poly_a,prime);
		bmodp = (int)mpz_tdiv_ui(poly->mpz_poly_b,prime);

		//find a^-1 mod p = inv(a mod p) mod p
        if (sconf->knmod8 == 1)
        {
            inv = modinv_1(2 * amodp, prime);
        }
        else
        {
            inv = modinv_1(amodp, prime);
        }

		COMPUTE_FIRST_ROOTS

        // Barrett reduction for inv * root
        t2 = (uint64_t)inv * (uint64_t)root1;
        tmp = (t2 * fb->list->binv[i]) >> 32;
        t2 -= tmp * prime;
        root1 = (t2 >= prime) ? t2 - prime : t2;

        t2 = (uint64_t)inv * (uint64_t)root2;
        tmp = (t2 * fb->list->binv[i]) >> 32;
        t2 -= tmp * prime;
        root2 = (t2 >= prime) ? t2 - prime : t2;

		if (root2 < root1)
		{
			update_data.sm_firstroots1[i] = (uint16_t)root2;
			update_data.sm_firstroots2[i] = (uint16_t)root1;

			fb_p->root1[i] = (uint16_t)root2;
			fb_p->root2[i] = (uint16_t)root1;
			fb_n->root1[i] = (uint16_t)(prime - root1);
			fb_n->root2[i] = (uint16_t)(prime - root2);
		}
		else
		{
			update_data.sm_firstroots1[i] = (uint16_t)root1;
			update_data.sm_firstroots2[i] = (uint16_t)root2;

			fb_p->root1[i] = (uint16_t)root1;
			fb_p->root2[i] = (uint16_t)root2;
			fb_n->root1[i] = (uint16_t)(prime - root2);
			fb_n->root2[i] = (uint16_t)(prime - root1);
		}

#ifdef USE_SS_SEARCH

		if (using_ss_search)
		{
			// first poly b: sum of first n positive Bl
			bmodp = (int)mpz_tdiv_ui(dconf->polyb1, prime);

			// under these conditions polya is added to polyb.
			// here we account for that by adding amodp into bmodp.
			// only here in set1 enumeration.
			if (sconf->knmod8 == 1)
			{
				if (sconf->charcount == 1)
				{
					bmodp += amodp;
					if (bmodp >= prime)
					{
						bmodp -= prime;
					}
				}
			}

			//COMPUTE_FIRST_ROOTS;
			root1 = (int)(modsqrt[i]) - bmodp;
			root2 = (int)(prime - modsqrt[i]) - bmodp;
			if (root1 < 0) root1 += prime;
			if (root2 < 0) root2 += prime;

			dconf->firstroot1a[i] = (int)((uint64_t)inv * (uint64_t)root1 % (uint64_t)prime);
			dconf->firstroot1b[i] = (int)((uint64_t)inv * (uint64_t)root2 % (uint64_t)prime);

			bmodp = (int)mpz_tdiv_ui(dconf->polyb2, prime);

			// - (B1 + B2 + B3 + ... Bs-1)
			bmodp = prime - bmodp;

			dconf->firstroot2[i] = (uint32_t)((uint64_t)inv * (uint64_t)bmodp % (uint64_t)prime);
		}
#endif
		//for this factor base prime, compute the rootupdate value for all s
		//Bl values.  amodp holds a^-1 mod p
		//the rootupdate value is given by 2*Bj*amodp
		//Bl[j] now holds 2*Bl
		for (j=0;j<s;j++)
		{
			x = (int)mpz_tdiv_ui(dconf->Bl[j],prime);

			// x * inv % prime
            t2 = (uint64_t)inv * (uint64_t)x;
            tmp = (t2 * fb->list->binv[i]) >> 32;
            t2 -= tmp * prime;
            x = (t2 >= prime) ? t2 - prime : t2;

			rootupdates[(j)*fb->B+i] = x;
			dconf->sm_rootupdates[(j)*fb->med_B+i] = (uint16_t)x;
		}
	}

	check_bound = fb->med_B + BUCKET_ALLOC/2;
	logp = fb->list->logprime[fb->med_B-1];
	for (i=fb->med_B;i<fb->large_B;i++)
	{
        CHECK_NEW_SLICE(i);

		prime = fb->list->prime[i];
		root1 = modsqrt[i];
		root2 = prime - root1; 

		amodp = (int)mpz_tdiv_ui(poly->mpz_poly_a,prime);
		bmodp = (int)mpz_tdiv_ui(poly->mpz_poly_b,prime);

		//find a^-1 mod p = inv(a mod p) mod p
        if (sconf->knmod8 == 1)
        {
            inv = modinv_1(2 * amodp, prime);
        }
        else
        {
            inv = modinv_1(amodp, prime);
        }

        COMPUTE_FIRST_ROOTS

        root1 = (uint32_t)((uint64_t)inv * (uint64_t)root1 % (uint64_t)prime);
        root2 = (uint32_t)((uint64_t)inv * (uint64_t)root2 % (uint64_t)prime);
		
		update_data.firstroots1[i] = root1;
		update_data.firstroots2[i] = root2;
        nroot1 = (prime - root1);
        nroot2 = (prime - root2);

		FILL_ONE_PRIME_LOOP_P(i);
		FILL_ONE_PRIME_LOOP_N(i);

#ifdef USE_SS_SEARCH

		if (using_ss_search)
		{
			// first poly b: sum of first n positive Bl
			bmodp = (int)mpz_tdiv_ui(dconf->polyb1, prime);

			// under these conditions polya is added to polyb.
			// here we account for that by adding amodp into bmodp.
			// only here in set1 enumeration.
			if (sconf->knmod8 == 1)
			{
				if (sconf->charcount == 1)
				{
					bmodp += amodp;
					if (bmodp >= prime)
					{
						bmodp -= prime;
					}
				}
			}

			//COMPUTE_FIRST_ROOTS;
			root1 = (int)(modsqrt[i]) - bmodp;
			root2 = (int)(prime - modsqrt[i]) - bmodp;
			if (root1 < 0) root1 += prime;
			if (root2 < 0) root2 += prime;

			dconf->firstroot1a[i] = (int)((uint64_t)inv * (uint64_t)root1 % (uint64_t)prime);
			dconf->firstroot1b[i] = (int)((uint64_t)inv * (uint64_t)root2 % (uint64_t)prime);

			bmodp = (int)mpz_tdiv_ui(dconf->polyb2, prime);

			// - (B1 + B2 + B3 + ... Bs-1)
			bmodp = prime - bmodp;

			dconf->firstroot2[i] = (uint32_t)((uint64_t)inv * (uint64_t)bmodp % (uint64_t)prime);
		}
#endif

		//for this factor base prime, compute the rootupdate value for all s
		//Bl values.  amodp holds a^-1 mod p
		//the rootupdate value is given by 2*Bj*amodp
		//Bl[j] now holds 2*Bl
		for (j=0;j<s;j++)
		{
			x = (int)mpz_tdiv_ui(dconf->Bl[j], prime);
			x = (int)((int64_t)x * (int64_t)inv % (int64_t)prime);
            rootupdates[(j)*fb->B + i] = x;
		}
	}
    
	logp = fb->list->logprime[fb->large_B-1];
    for (i = fb->large_B; i < fb->B; i++)
    {
        CHECK_NEW_SLICE(i);

        prime = fb->list->prime[i];
        root1 = modsqrt[i];
        root2 = prime - root1;
		uint8_t logp = fb->list->logprime[i];

        amodp = (int)mpz_tdiv_ui(poly->mpz_poly_a, prime);
        bmodp = (int)mpz_tdiv_ui(poly->mpz_poly_b, prime);

        //find a^-1 mod p = inv(a mod p) mod p
        if (sconf->knmod8 == 1)
        {
            inv = modinv_1(2 * amodp, prime);
        }
        else
        {
            inv = modinv_1(amodp, prime);
        }

        COMPUTE_FIRST_ROOTS;

        root1 = (uint32_t)((uint64_t)inv * (uint64_t)root1 % (uint64_t)prime);
        root2 = (uint32_t)((uint64_t)inv * (uint64_t)root2 % (uint64_t)prime);

        update_data.firstroots1[i] = root1;
        update_data.firstroots2[i] = root2;

        FILL_ONE_PRIME_P(i);

        root1 = (prime - root1);
        root2 = (prime - root2);

        FILL_ONE_PRIME_N(i);

#ifdef USE_SS_SEARCH

		if (using_ss_search)
		{
			// first poly b: sum of first n positive Bl
			bmodp = (int)mpz_tdiv_ui(dconf->polyb1, prime);

			// under these conditions polya is added to polyb.
			// here we account for that by adding amodp into bmodp.
			// only here in set1 enumeration.
			if (sconf->knmod8 == 1)
			{
				if (sconf->charcount == 1)
				{
					bmodp += amodp;
					if (bmodp >= prime)
					{
						bmodp -= prime;
					}
				}
			}

			//COMPUTE_FIRST_ROOTS;
			root1 = (int)(modsqrt[i]) - bmodp;
			root2 = (int)(prime - modsqrt[i]) - bmodp;
			if (root1 < 0) root1 += prime;
			if (root2 < 0) root2 += prime;

			dconf->firstroot1a[i] = (int)((uint64_t)inv * (uint64_t)root1 % (uint64_t)prime);
			dconf->firstroot1b[i] = (int)((uint64_t)inv * (uint64_t)root2 % (uint64_t)prime);

			bmodp = (int)mpz_tdiv_ui(dconf->polyb2, prime);

			// - (B1 + B2 + B3 + ... Bs-1)
			bmodp = prime - bmodp;

			dconf->firstroot2[i] = (uint32_t)((uint64_t)inv * (uint64_t)bmodp % (uint64_t)prime);
		}
#endif

        //for this factor base prime, compute the rootupdate value for all s
        //Bl values.  amodp holds a^-1 mod p
        //the rootupdate value is given by 2*Bj*amodp
        //Bl[j] now holds 2*Bl
        //s is the number of primes in 'a'
        for (j = 0; j < s; j++)
        {
            x = (int)mpz_tdiv_ui(dconf->Bl[j], prime);
            x = (int)((int64_t)x * (int64_t)inv % (int64_t)prime);
            rootupdates[(j)*fb->B + i] = x;
        }
    }


    if (lp_bucket_p->list != NULL)
    {
#ifdef USE_BATCHPOLY_X2
        int pnum;
        pnum = (dconf->numB % dconf->poly_batchsize) - 1;
        if (pnum < 0)
            pnum += dconf->poly_batchsize;

        lp_bucket_p->num_slices_batch[pnum] = bound_index + 1;

#ifdef DEBUGPRINT_BATCHPOLY
        printf("num slices after first poly (%d,%d) bucket sieve: %u\n", 
			dconf->numB, pnum, bound_index + 1);
#endif

#else
        lp_bucket_p->num_slices = bound_index + 1;
#endif
    }

	return;
}

