// SPDX-License-Identifier: LGPL-2.1-only OR MPL-1.1
/*
 * Copyright 2009  Evangelos Katsikaros <vkatsikaros at yahoo dot gr>
 */

/*
 initial toy for redblack trees
*/


#include <2geom/rtree.h>

#include <time.h>
#include <vector>

#include <sstream>
#include <getopt.h>




//using std::vector;
using namespace Geom;
using namespace std;

sadfsdfasdfasdfa

int main(int argc, char **argv) {

	long test_seed = 1243716824;

	char* min_arg = NULL;
	char* max_arg = NULL;
	char* filename_arg = NULL;

	int set_min_max = 0;

	int c;

	while (1)
	{
		static struct option long_options[] =
		{
			/* These options set a flag. */
			/* These options don't set a flag.
			We distinguish them by their indices. */
			{"min-nodes",	required_argument,	0, 'n'},
			{"max-nodes",	required_argument,	0, 'm'},
			{"input-file",	required_argument,	0, 'f'},
			{"help",		no_argument,		0, 'h'},
			{0, 0, 0, 0}
		};
		/* getopt_long stores the option index here. */
		int option_index = 0;

		c = getopt_long (argc, argv, "n:m:f:h",
			long_options, &option_index);

		/* Detect the end of the options. */
		if (c == -1){
			break;
		}

		switch (c)
		{
			case 'n':
			min_arg = optarg;
			set_min_max += 1;
			break;


			case 'm':
			max_arg = optarg;	
			set_min_max += 2;
			break;

			case 'f':
			filename_arg = optarg;	
			set_min_max += 3;
			break;


			case 'h':
			std::cerr << "Usage:  " << argv[0] << " options\n" << std::endl ;
			std::cerr << 
				   "  -n  --min-nodes=NUMBER   minimum number in node.\n" <<
				   "  -m  --max-nodes=NUMBER   maximum number in node.\n" <<
				   "  -f  --max-nodes=NUMBER   maximum number in node.\n" <<
				   "  -h  --help               Print this help.\n" << std::endl;
			exit(1);
			break;


			case '?':
			/* getopt_long already printed an error message. */
			break;

			default:
			abort ();
		}
	}

	unsigned rmin = 0;
	unsigned rmax = 0;

	if(	set_min_max == 6 ){
		stringstream s1( min_arg );
		s1 >> rmin;

		stringstream s2( max_arg );
		s2 >> rmax;


		if( rmax <= rmin || rmax < 2 || rmin < 1 ){
			std::cerr << "Rtree set to 2, 3" << std::endl ;
			rmin = 2;
			rmax = 3;			
		}
	}
	else{
		std::cerr << "Rtree set to 2, 3 ." << std::endl ;
		rmin = 2;
		rmax = 3;
	}



	std::cout << "rmin: " << rmin << "  rmax:" << rmax << "  filename_arg:" << filename_arg << std::endl;

	RTree rtree( rmin, rmax, QUADRATIC_SPIT );

	srand(1243716824);	
	rand() % 10;

    return 0;
}
