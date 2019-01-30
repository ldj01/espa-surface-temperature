#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <math.h>
#include <getopt.h>
#include <errno.h>
#include <string.h>
#include <float.h>


#include "const.h"
#include "utilities.h"
#include "input.h"
#include "st_types.h"
#include "output.h"
#include "intermediate_data.h"
#include "calculate_atmospheric_parameters.h"

/*****************************************************************************
DESCRIPTION: Using values produced by MODTRAN runs at grid points, calculate
atmospheric transmission, upwelled radiance, and downwelled radiance for each
pixel in the Landsat scene.  Also, create bands with these values.  A thermal
radiance band is also created based on a Landsat thermal band and parameters.
*****************************************************************************/

/* calculate_point_atmospheric_parameters functions */

/*****************************************************************************
METHOD:  planck_eq

PURPOSE: Using Planck's equation to calculate radiance at each wavelength for
         current temperature.
*****************************************************************************/
static void planck_eq
(
    double *wavelength, /* I: Each wavelength */
    int num_elements,   /* I: Number of wavelengths to calculate */
    double temperature, /* I: The temperature to calculate for */
    double *bb_radiance /* O: the blackbody results for each wavelength */
)
{
    int i;
    double lambda;

    /* Planck Const hecht pg, 585 ## units: Js */
    double PLANCK_CONST = 6.6260755e-34;

    /* Boltzmann Gas Const halliday et 2001 -- units: J/K */
    double BOLTZMANN_GAS_CONST = 1.3806503e-23;

    /* Speed of Light -- units: m/s */
    double SPEED_OF_LIGHT = 299792458.0;
    double SPEED_OF_LIGHT_SQRD = SPEED_OF_LIGHT * SPEED_OF_LIGHT;

    for (i = 0; i < num_elements; i++)
    {
        /* Lambda intervals of spectral response locations microns units: m */
        lambda = wavelength[i] * 1e-6;

        /* Compute the Planck Blackbody Eq [W/m^2 sr um] */
        bb_radiance[i] = 2.0 * PLANCK_CONST * SPEED_OF_LIGHT_SQRD
                         * 1e-6 * pow(lambda, -5.0)
                         * (1.0 / (exp ((PLANCK_CONST * SPEED_OF_LIGHT)
                                         / (lambda
                                            * BOLTZMANN_GAS_CONST
                                            * temperature))
                                   - 1.0));

        /* Convert to W/cm^2 sr micron to match modtran units */
        /* br / (100 * 100) == br * 1e-4 */
        bb_radiance[i] *= 1e-4;
    }
}


/*****************************************************************************
MODULE:  spline

PURPOSE: spline constructs a cubic spline given a set of x and y values,
         through these values.

RETURN: SUCCESS
        FAILURE
*****************************************************************************/
static int spline
(
    double *x,
    double *y,
    int n,
    double yp1,
    double ypn,
    double *y2
)
{
    char FUNC_NAME[] = "spline";
    int i;
    double p;
    double qn;
    double sig;
    double un;
    double *u = NULL;

    u = malloc ((unsigned) (n - 1) * sizeof (double));
    if (u == NULL)
    {
        RETURN_ERROR ("Can't allocate memory", FUNC_NAME, FAILURE);
    }

    /* Set the lower boundary */
    if (yp1 > 0.99e30)
    {
        /* To be "natural" */
        y2[0] = 0.0;
        u[0] = 0.0;
    }
    else
    {
        /* To have a specified first derivative */
        y2[0] = -0.5;
        u[0] = (3.0 / (x[1] - x[0]))
               * ((y[1] - y[0]) / (x[1] - x[0]) - yp1);
    }

    /* Set the upper boundary */
    if (ypn > 0.99e30)
    {
        /* To be "natural" */
        qn = 0.0;
        un = 0.0;
    }
    else
    {
        /* To have a specified first derivative */
        qn = 0.5;
        un = (3.0 / (x[n - 1] - x[n - 2]))
             * (ypn - (y[n - 1] - y[n - 2]) / (x[n - 1] - x[n - 2]));
    }

    /* Perform decomposition of the tridiagonal algorithm */
    for (i = 1; i <= n - 2; i++)
    {
        sig = (x[i] - x[i - 1]) / (x[i + 1] - x[i - 1]);

        p = sig * y2[i - 1] + 2.0;

        y2[i] = (sig - 1.0) / p;

        u[i] = (y[i + 1] - y[i]) / (x[i + 1] - x[i])
               - (y[i] - y[i - 1]) / (x[i] - x[i - 1]);

        u[i] = (6.0 * u[i] / (x[i + 1] - x[i - 1]) - sig * u[i - 1]) / p;
    }
    y2[n - 1] = (un - qn * u[n - 2]) / (qn * y2[n - 2] + 1.0);

    /* Perform the backsubstitution of the tridiagonal algorithm */
    for (i = n - 2; i >= 0; i--)
    {
        y2[i] = y2[i] * y2[i + 1] + u[i];
    }

    free (u);

    return SUCCESS;
}


/*****************************************************************************
MODULE:  splint

PURPOSE: splint uses the cubic spline generated with spline to interpolate
         values in the XY table
*****************************************************************************/
static void splint
(
    double *xa,
    double *ya,
    double *y2a,
    int n,
    double x,
    double *y
)
{
    int k;
    double h;
    double a;
    static int splint_klo = -1;
    static int splint_khi = -1;
    static double one_sixth = (1.0 / 6.0); /* To remove a division */

    if (splint_klo < 0)
    {
        splint_klo = 0;
        splint_khi = n - 1;
    }
    else
    {
        if (x < xa[splint_klo])
            splint_klo = 0;
        if (x > xa[splint_khi])
            splint_khi = n - 1;
    }

    while (splint_khi - splint_klo > 1)
    {
        k = (splint_khi + splint_klo) >> 1;

        if (xa[k] > x)
            splint_khi = k;
        else
            splint_klo = k;
    }

    h = xa[splint_khi] - xa[splint_klo];

    if (h == 0.0)
    {
        *y = 0.0;
    }
    else
    {
        a = (xa[splint_khi] - x) / h;

        /*  The equation used below is the following, simplified:

            b = 1 - a;
            *y = a * ya[splint_klo]
               + b * ya[splint_khi]
               + ((a * a * a - a) * y2a[splint_klo]
               + (b * b * b - b) * y2a[splint_khi]) * (h * h) * one_sixth; */

        *y = ya[splint_khi] + a*(ya[splint_klo] - ya[splint_khi])
            + one_sixth*h*h*a*(a - 1)*((a + 1)*y2a[splint_klo] +
                                       (2 - a)*y2a[splint_khi]);
    }
}


/*****************************************************************************
MODULE:  int_tabulated

PURPOSE: This function integrates a tabulated set of data { x(i) , f(i) },
         on the closed interval [min(X) , max(X)].

RETURN: SUCCESS
        FAILURE

NOTE: x and f are assumed to be in sorted order (min(x) -> max(x))
*****************************************************************************/
static int int_tabulated
(
    double *x,         /*I: Tabulated X-value data */
    double *f,         /*I: Tabulated F-value data */
    int nums,          /*I: Number of points */
    double *result_out /*O: Integrated result */
)
{
    char FUNC_NAME[] = "int_tabulated";
    double *temp = NULL;
    double *z = NULL;
    double xmin;
    double xmax;
    int i;
    int *ii = NULL;
    int ii_count;
    double h;
    double result;
    int segments;

    /* Figure out the number of segments needed */
    segments = nums - 1;
    while (segments % 4 != 0)
        segments++;

    /* Determine how many iterations are needed  */
    ii_count = (int) ((segments) / 4);

    /* Determine the min and max */
    xmin = x[0];
    xmax = x[nums - 1];

    /* Determine the step size */
    h = (xmax - xmin) / segments;

    /* Allocate memory */
    temp = malloc (nums * sizeof (double));
    if (temp == NULL)
    {
        RETURN_ERROR ("Allocating temp memory", FUNC_NAME, FAILURE);
    }

    z = malloc ((segments+1) * sizeof (double));
    if (z == NULL)
    {
        RETURN_ERROR ("Allocating z memory", FUNC_NAME, FAILURE);
    }

    ii = malloc (ii_count * sizeof (int));
    if (ii == NULL)
    {
        RETURN_ERROR ("Allocating ii memory", FUNC_NAME, FAILURE);
    }

    /* Interpolate spectral response over wavelength */
    /* Using 1e30 forces generation of a natural spline and produces nearly
       the same results as IDL */
    if (spline (x, f, nums, 1e30, 1e30, temp) != SUCCESS)
    {
        RETURN_ERROR ("Failed during spline", FUNC_NAME, FAILURE);
    }

    /* Call splint for interpolations. one-based arrays are considered */
    for (i = 0; i < segments+1; i++)
    {
        splint (x, f, temp, nums, h*i+xmin, &z[i]);
    }

    /* Get the 5-points needed for Newton-Cotes formula */
    for (i = 0; i < ii_count; i++)
    {
        ii[i] = (i + 1) * 4;
    }

    /* Compute the integral using the 5-point Newton-Cotes formula */
    result = 0.0;
    for (i = 0; i < ii_count; i++)
    {
        double *z_ptr = &z[ii[i] - 4];

        result += 14*(z_ptr[0] + z_ptr[4]) + 64*(z_ptr[1] + z_ptr[3])
                + 24*z_ptr[2];
    }

    /* Assign the results to the output */
    *result_out = result*h/45;

    free (temp);
    free (z);
    free (ii);

    return SUCCESS;
}


/*****************************************************************************
MODULE:  calculate_lt

PURPOSE: Calculate blackbody radiance from temperature using spectral response
         function.

RETURN: SUCCESS
        FAILURE
*****************************************************************************/
static int calculate_lt
(
    double temperature,         /*I: temperature */
    double **spectral_response, /*I: spectral response function */
    int num_srs,                /*I: number of spectral response points */
    double *radiance            /*O: blackbody radiance */
)
{
    char FUNC_NAME[] = "calculate_lt";
    int i;
    double rs_integral;
    double temp_integral;
    double *product;
    double *blackbody_radiance;

    /* Allocate memory */
    blackbody_radiance = malloc (num_srs * sizeof (double));
    if (blackbody_radiance == NULL)
    {
        RETURN_ERROR ("Allocating blackbody_radiance memory", FUNC_NAME,
                      FAILURE);
    }

    product = malloc (num_srs * sizeof (double));
    if (product == NULL)
    {
        RETURN_ERROR ("Allocating product memory", FUNC_NAME, FAILURE);
    }

    /* integrate spectral response over wavelength */
    if (int_tabulated (spectral_response[0], spectral_response[1], num_srs,
                       &rs_integral) != SUCCESS)
    {
        RETURN_ERROR ("Calling int_tabulated\n", FUNC_NAME, FAILURE);
    }

    /* Use planck's blackbody radiance equation to calculate radiance at each
       wavelength for the current temperature */
    planck_eq (spectral_response[0], num_srs, temperature, blackbody_radiance);

    /* Multiply the calculated planck radiance by the spectral response and
       integrate over wavelength to get one number for current temp */
    for (i = 0; i < num_srs; i++)
    {
        product[i] = blackbody_radiance[i] * spectral_response[1][i];
    }

    if (int_tabulated (spectral_response[0], product, num_srs,
                       &temp_integral) != SUCCESS)
    {
        RETURN_ERROR ("Calling int_tabulated\n", FUNC_NAME, FAILURE);
    }

    /* Divide above result by integral of spectral response function */
    *radiance = temp_integral / rs_integral;

    /* Free allocated memory */
    free (blackbody_radiance);
    free (product);

    return SUCCESS;
}


/*****************************************************************************
MODULE:  linear_interpolate_over_modtran

PURPOSE: Simulate IDL (interpol) function for ST.
*****************************************************************************/
static void linear_interpolate_over_modtran
(
    double (*modtran)[4], /* I: The MODTRAN data - provides both the a and b */
    int index,        /* I: The MODTRAN temperatur to use for a */
    double *c,        /* I: The Landsat wavelength grid points */
    int num_in,       /* I: Number of input data and grid points*/
    int num_out,      /* I: Number of output grid points */
    double *x         /* O: Interpolated output results */
)
{
    int i;
    int o;

    double d1 = 0.0;
    double d2 = 0.0;
    double g;
    double g1 = 0.0;
    double g2 = 0.0;

    int a = index; /* MODTRAN radiance for specififc temp */
    int b = 0;     /* MODTRAN wavelength */

    for (o = 0; o < num_out; o++)
    {
        g = c[o];

        /* Initialize to the first two */
        d1 = modtran[0][a];
        d2 = modtran[1][a];
        g1 = modtran[0][b];
        g2 = modtran[1][b];

        for (i = 0; i < num_in-1; i++)
        {
            if (g <= modtran[i][b] && g > modtran[i+1][b])
            {
                /* Found it in the middle of the data */
                d1 = modtran[i][a];
                d2 = modtran[i+1][a];
                g1 = modtran[i][b];
                g2 = modtran[i+1][b];
                break;
            }
        }

        if (i == num_in-1)
        {
            /* Less than the last so use the last two */
            d1 = modtran[i-1][a];
            d2 = modtran[i][a];
            g1 = modtran[i-1][b];
            g2 = modtran[i][b];
        }

        /* Apply the formula for linear interpolation */
        x[o] = d1 + (g - g1) / (g2 - g1) * (d2 - d1);
    }
}


/*****************************************************************************
MODULE:  calculate_lobs

PURPOSE: Calculate observed radiance from MODTRAN results and the spectral
         response function.

RETURN: SUCCESS
        FAILURE
*****************************************************************************/
static int calculate_lobs
(
    double (*modtran)[4],       /*I: MODTRAN results with wavelengths */
    double **spectral_response, /*I: spectral response function */
    int num_entries,            /*I: number of MODTRAN points */
    int num_srs,                /*I: number of spectral response points */
    int index,                  /*I: column index for data be used */
    double *radiance            /*O: LOB outputs */
)
{
    char FUNC_NAME[] = "calculate_lobs";
    int i;
    double *temp_rad;
    double rs_integral;
    double temp_integral;
    double *product;

    /* Allocate memory */
    temp_rad = malloc (num_srs * sizeof (double));
    if (temp_rad == NULL)
    {
        RETURN_ERROR ("Allocating temp_rad memory", FUNC_NAME, FAILURE);
    }

    product = malloc (num_srs * sizeof (double));
    if (product == NULL)
    {
        RETURN_ERROR ("Allocating product memory", FUNC_NAME, FAILURE);
    }

    /* Integrate spectral response over wavelength */
    if (int_tabulated (spectral_response[0], spectral_response[1], num_srs,
                       &rs_integral) != SUCCESS)
    {
        RETURN_ERROR ("Calling int_tabulated\n", FUNC_NAME, FAILURE);
    }

    /* Interpolate MODTRAN radiance to Landsat wavelengths */
    linear_interpolate_over_modtran (modtran, index, spectral_response[0],
                                     num_entries, num_srs, temp_rad);

    /* Multiply the calculated radiance by the spectral response and integrate
       over wavelength to get one number for current temperature */
    for (i = 0; i < num_srs; i++)
    {
        product[i] = temp_rad[i] * spectral_response[1][i];
    }

    if (int_tabulated (spectral_response[0], product, num_srs,
                       &temp_integral) != SUCCESS)
    {
        RETURN_ERROR ("Calling int_tabulated\n", FUNC_NAME, FAILURE);
    }

    /* Divide above result by integral of spectral response function */
    *radiance = temp_integral / rs_integral;

    /* Free allocated memory */
    free (temp_rad);
    free (product);

    return SUCCESS;
}


/*****************************************************************************
METHOD:  calculate_point_atmospheric_parameters

PURPOSE: Generate transmission, upwelled radiance, and downwelled radiance at
         each height for each NARR point that is used.

RETURN: SUCCESS
        FAILURE
*****************************************************************************/
static int calculate_point_atmospheric_parameters
(
    Input_Data_t *input,       /* I: Input structure */
    GRID_POINTS *grid_points,  /* I: The coordinate points */
    MODTRAN_POINTS *modtran_results /* I/O: Atmospheric parameters from 
                                   MODTRAN */
)
{
    char FUNC_NAME[] = "calculate_point_atmospheric_parameters";

    FILE *fd;
    FILE *used_points_fd;

    int i;
    int j;
    int entry;

    double *spectral_response[2];
    double temp_radiance_0;
    double obs_radiance_0;
    double temp_radiance_273;
    double temp_radiance_310;
    double delta_radiance_inv; /* inverse of radiance differences; used to
                                  compute transmittance and upwelled radiance */
    int counter;
    int index;
    int num_entries;   /* Number of MODTRAN output results to read and use */
    int num_srs;       /* Number of spectral response values available */

    char *st_data_dir = NULL;
    char current_file[PATH_MAX]; /* Used for MODTRAN info (input), MODTRAN data
                          (input), and atmospheric parameters (output) files */
    char srs_file_path[PATH_MAX];
    char msg[PATH_MAX];

    double modtran_wavelength;
    double modtran_radiance;
    double zero_temp;
    double (*current_data)[4] = NULL;
    int max_radiance_record_count = 0; /* max number of radiance records read */
    double y_0;
    double y_1;
    double tau; /* Transmission */
    double lu;  /* Upwelled Radiance */
    double ld;  /* Downwelled Radiance */

    /* Temperature and albedo */
    int temperature[3] = { 273, 310, 000 };
    double albedo[3] = { 0.0, 0.0, 0.1 };


    st_data_dir = getenv ("ST_DATA_DIR");
    if (st_data_dir == NULL)
    {
        RETURN_ERROR ("ST_DATA_DIR environment variable is not set",
                      FUNC_NAME, FAILURE);
    }

    /* Allocate memory for maximum spectral response count */
    spectral_response[0] = malloc(MAX_SRS_COUNT*sizeof(double));
    spectral_response[1] = malloc(MAX_SRS_COUNT*sizeof(double));
    if (spectral_response[0] == NULL || spectral_response[1] == NULL)
    {
        RETURN_ERROR ("Allocating spectral_response memory",
                      FUNC_NAME, FAILURE);
    }

    /* Determine the spectral response file to read */
    if (input->meta.instrument == INST_TM
        && input->meta.satellite == SAT_LANDSAT_4)
    {
        num_srs = L4_TM_SRS_COUNT;

        snprintf (srs_file_path, sizeof (srs_file_path),
                  "%s/%s", st_data_dir, "L4_Spectral_Response.txt");
    }
    else if (input->meta.instrument == INST_TM
        && input->meta.satellite == SAT_LANDSAT_5)
    {
        num_srs = L5_TM_SRS_COUNT;

        snprintf (srs_file_path, sizeof (srs_file_path),
                  "%s/%s", st_data_dir, "L5_Spectral_Response.txt");
    }
    else if (input->meta.instrument == INST_ETM
             && input->meta.satellite == SAT_LANDSAT_7)
    {
        num_srs = L7_TM_SRS_COUNT;

        snprintf (srs_file_path, sizeof (srs_file_path),
                  "%s/%s", st_data_dir, "L7_Spectral_Response.txt");
    }
    else if (input->meta.instrument == INST_OLI_TIRS
             && input->meta.satellite == SAT_LANDSAT_8)
    {
        num_srs = L8_OLITIRS_SRS_COUNT;

        snprintf (srs_file_path, sizeof (srs_file_path),
                  "%s/%s", st_data_dir, "L8_Spectral_Response.txt");
    }
    else
    {
        RETURN_ERROR ("invalid instrument type", FUNC_NAME, FAILURE);
    }

    /* Read the selected spectral response file */
    snprintf (msg, sizeof (msg),
              "Reading Spectral Response File [%s]", srs_file_path);
    LOG_MESSAGE (msg, FUNC_NAME);
    fd = fopen (srs_file_path, "r");
    if (fd == NULL)
    {
        RETURN_ERROR ("Can't open Spectral Response file", FUNC_NAME, FAILURE);
    }

    for (i = 0; i < num_srs; i++)
    {
        if (fscanf (fd, "%lf %lf%*c", &spectral_response[0][i],
                    &spectral_response[1][i]) == EOF)
        {
            RETURN_ERROR ("Failed reading spectral response file",
                          FUNC_NAME, FAILURE);
        }
    }
    fclose (fd);

    /* Calculate Lt for each specific temperature */
    if (calculate_lt (273, spectral_response, num_srs, &temp_radiance_273)
        != SUCCESS)
    {
        RETURN_ERROR ("Calling calculate_lt for 273K", FUNC_NAME, FAILURE);
    }
    if (calculate_lt (310, spectral_response, num_srs, &temp_radiance_310)
        != SUCCESS)
    {
        RETURN_ERROR ("Calling calculate_lt for 310K", FUNC_NAME, FAILURE);
    }

    /* Compute the multiplier for the transmittance and upwelled radiance
       calculations in the following loop. */
    delta_radiance_inv = 1/(temp_radiance_310 - temp_radiance_273);

    /* Output information about the used points, primarily useful for
       plotting them against the scene */
    used_points_fd = fopen ("used_points.txt", "w");
    if (used_points_fd == NULL)
    {
        RETURN_ERROR ("Can't open used_points.txt file",
                      FUNC_NAME, FAILURE);
    }

    /* Iterate through all grid points and heights */
    counter = 0;
    for (i = 0; i < grid_points->count; i++)
    {
        GRID_POINT *grid_point = &grid_points->points[i];
        MODTRAN_POINT *modtran_point = & modtran_results->points[i];

        /* Don't process the points that didn't have a MODTRAN run. */
        if (!modtran_point->ran_modtran)
        {
            continue;
        }

        fprintf (used_points_fd, "\"%d\"|\"%f\"|\"%f\"\n",
                 i, grid_point->map_x, grid_point->map_y);

        for (j = 0; j < modtran_point->count; j++)
        {
            /* Read the st_modtran.info file for the 000 execution
               (when MODTRAN is run at 0K)
               We read the zero_temp from this file, and also the record count
               The record count is the same for all three associated runs */
            snprintf(current_file, sizeof(current_file), 
                "%03d_%03d_%03d_%03d/%1.3f/000/0.1/st_modtran.hdr",
                grid_point->row, grid_point->col, grid_point->narr_row,
                grid_point->narr_col,
                modtran_point->elevations[j].elevation_directory);

            fd = fopen (current_file, "r");
            if (fd == NULL)
            {
                snprintf (msg, sizeof (msg),
                          "Can't open MODTRAN information file [%s]",
                           current_file);
                RETURN_ERROR (msg, FUNC_NAME, FAILURE);
            }
            /* Retrieve the temperature from this lowest atmospheric layer */
            if (fscanf (fd, "%*s %lf%*c", &zero_temp) != 1)
            {
                RETURN_ERROR ("End of file (EOF) is met before"
                              " reading TARGET_PIXEL_SURFACE_TEMPERATURE",
                              FUNC_NAME, FAILURE);
            }
            /* Determine number of entries in current file */
            if (fscanf (fd, "%*s %d%*c", &num_entries) != 1)
            {
                RETURN_ERROR ("End of file (EOF) is met before"
                              " reading RADIANCE_RECORD_COUNT",
                              FUNC_NAME, FAILURE);
            }
            fclose (fd);

            /* For each height, read in radiance information for three
               MODTRAN runs.  Columns of array are organized as follows:
               wavelength | 273,0.0 | 310,0.0 | 000,0.1 */
            if (num_entries > max_radiance_record_count)
            {
                max_radiance_record_count = num_entries;
                current_data = realloc(current_data,
                                       num_entries*sizeof(double[4]));
                if (current_data == NULL)
                {
                    RETURN_ERROR ("Allocating current_data memory",
                                  FUNC_NAME, FAILURE);
                }
            }

            /* Iterate through the three pairs of parameters */
            for (index = 1; index < 4; index++)
            {
                /* Define MODTRAN data file */
                snprintf(current_file, sizeof(current_file), 
                    "%03d_%03d_%03d_%03d/%1.3f/%03d/%1.1f/st_modtran.data",
                    grid_point->row, grid_point->col, grid_point->narr_row,
                    grid_point->narr_col,
                    modtran_point->elevations[j].elevation_directory,
                    temperature[index - 1],
                    albedo[index - 1]);

                fd = fopen (current_file, "r");
                if (fd == NULL)
                {
                    RETURN_ERROR ("Can't open MODTRAN data file",
                                  FUNC_NAME, FAILURE);
                }
                for (entry = 0; entry < num_entries; entry++)
                {
                    if (fscanf (fd, "%lf %lf%*c",
                                &modtran_wavelength, &modtran_radiance)
                        != 2)
                    {
                        RETURN_ERROR ("Failed reading st_modtran.dat lines",
                                      FUNC_NAME, FAILURE);
                    }

                    /* If we are on the first file set the wavelength value
                       for the data array */
                    if (index == 1)
                    {
                        current_data[entry][0] = modtran_wavelength;
                    }
                    /* Place radiance into data array for current point at
                       current height */
                    current_data[entry][index] = modtran_radiance;

                }
                fclose (fd);

                counter++;
            }

            /* Parameters from 3 MODTRAN runs
               Lobs = Lt*tau + Lu; m = tau; b = Lu; */
            if (calculate_lobs (current_data, spectral_response,
                                num_entries, num_srs, 1, &y_0)
                != SUCCESS)
            {
                RETURN_ERROR ("Calling calculate_lobs for height y_0",
                              FUNC_NAME, FAILURE);
            }

            if (calculate_lobs (current_data, spectral_response,
                                num_entries, num_srs, 2, &y_1)
                != SUCCESS)
            {
                RETURN_ERROR ("Calling calculate_lobs for height y_1",
                              FUNC_NAME, FAILURE);
            }

            tau = (y_1 - y_0)*delta_radiance_inv; /* Transmittance */
            lu = (temp_radiance_310*y_0 - temp_radiance_273*y_1)
               * delta_radiance_inv;  /* Upwelled Radiance */

            /* Determine Lobs and Lt when MODTRAN was run at 0K - calculate 
               downwelled */
            if (calculate_lt (zero_temp, spectral_response, num_srs,
                              &temp_radiance_0) != SUCCESS)
            {
                RETURN_ERROR ("Calling calculate_lt for zero temp (0Kelvin)",
                              FUNC_NAME, FAILURE);
            }

            if (calculate_lobs (current_data, spectral_response,
                                num_entries, num_srs, 3, &obs_radiance_0)
                != SUCCESS)
            {
                RETURN_ERROR ("Calling calculate_lobs for (0Kelvin)",
                              FUNC_NAME, FAILURE);
            }

            /* Calculate the downwelled radiance. These are all equivalent:
               Ld = (((Lobs - Lu) / tau)
                     - (Lt * WATER_EMISSIVITY)) / (1.0 - WATER_EMISSIVITY)
               Ld = (((Lobs - Lu) / tau)
                     - (Lt * WATER_EMISSIVITY)) / WATER_ALBEDO
               Ld = (((Lobs - Lu) / tau)
                     - (Lt * WATER_EMISSIVITY)) * INV_WATER_ALBEDO */
            ld = (((obs_radiance_0 - lu) / tau)
                  - (temp_radiance_0 * WATER_EMISSIVITY)) * INV_WATER_ALBEDO;

            /* Place results into MODTRAN results array */
            modtran_point->elevations[j].transmission = tau;
            modtran_point->elevations[j].upwelled_radiance = lu;
            modtran_point->elevations[j].downwelled_radiance = ld;
        } /* END - modtran_point->count loop */
    } /* END - count loop */
    fclose (used_points_fd);

    /* Free allocated memory */
    free(current_data);
    current_data = NULL;
    free(spectral_response[0]);
    spectral_response[0] = NULL;
    free(spectral_response[1]);
    spectral_response[1] = NULL;

    /* Write atmospheric transmission, upwelled radiance, and downwelled 
       radiance for each elevation for each point to a file */
    snprintf (current_file, sizeof (current_file),
              "atmospheric_parameters.txt");
    snprintf (msg, sizeof (msg),
              "Creating Atmospheric Parameters File = [%s]\n", current_file);
    LOG_MESSAGE (msg, FUNC_NAME);
    fd = fopen (current_file, "w");
    if (fd == NULL)
    {
        RETURN_ERROR ("Can't open atmospheric_parameters.txt file",
                      FUNC_NAME, FAILURE);
    }
    for (i = 0; i < grid_points->count; i++)
    {
        MODTRAN_POINT *modtran_point = & modtran_results->points[i];

        /* Only write parameters for grid points where MODTRAN was run */
        if (!modtran_point->ran_modtran)
        {
            continue;
        }

        for (j = 0; j < modtran_point->count; j++)
        {
            fprintf (fd, "%f,%f,%12.9f,%12.9f,%12.9f,%12.9f\n",
                 modtran_point->lat,
                 modtran_point->lon,
                 modtran_point->elevations[j].elevation,
                 modtran_point->elevations[j].transmission,
                 modtran_point->elevations[j].upwelled_radiance,
                 modtran_point->elevations[j].downwelled_radiance);
        }
    }
    fclose (fd);

    return SUCCESS;
}


/******************************************************************************
METHOD:  qsort_grid_compare_function

PURPOSE: A qsort routine that can be used with the GRID_ITEM items to sort by
         distance

RETURN: int: -1 (a<b), 1 (b<a), 0 (a==b) 
******************************************************************************/
static int qsort_grid_compare_function
(
    const void *grid_item_a,
    const void *grid_item_b
)
{
    double a = (*(GRID_ITEM*)grid_item_a).distance;
    double b = (*(GRID_ITEM*)grid_item_b).distance;

    if (a < b)
        return -1;
    else if (b < a)
        return 1;

    return 0;
}


/******************************************************************************
METHOD:  haversine_distance

PURPOSE: Calculates the great-circle distance between 2 points in meters.
         The points are given in decimal degrees.  The Haversine formula
         is used. 

RETURN: double - The great-circle distance in meters between the points.

NOTE: This is based on the haversine_distance function in the ST Python
      scripts.
******************************************************************************/
static double haversine_distance
(
    double lon_1,  /* I: the longitude for the first point */
    double lat_1,  /* I: the latitude for the first point */
    double lon_2,  /* I: the longitude for the second point */
    double lat_2   /* I: the latitude for the second point */
)
{

    double lat_1_radians; /* Latitude for first point in radians */
    double lat_2_radians; /* Latitude for second point in radians */
    double sin_lon;       /* Intermediate value */
    double sin_lat;       /* Intermediate value */
    double sin_lon_sqrd;  /* Intermediate value */
    double sin_lat_sqrd;  /* Intermediate value */

    /* Convert to radians */
    lat_1_radians = lat_1 * RADIANS_PER_DEGREE;
    lat_2_radians = lat_2 * RADIANS_PER_DEGREE;

    /* Figure out some sines */
    sin_lon = sin((lon_2 - lon_1)*0.5*RADIANS_PER_DEGREE);
    sin_lat = sin((lat_2_radians - lat_1_radians)*0.5);
    sin_lon_sqrd = sin_lon * sin_lon;
    sin_lat_sqrd = sin_lat * sin_lat;

    /* Compute and return the distance */
    return EQUATORIAL_RADIUS * 2 + asin(sqrt(sin_lat_sqrd 
        + cos(lat_1_radians) * cos(lat_2_radians) * sin_lon_sqrd));
}

/******************************************************************************
METHOD:  interpolate_to_height

PURPOSE: Interpolate to height of current pixel
******************************************************************************/
static void interpolate_to_height
(
    MODTRAN_POINT modtran_point, /* I: results from MODTRAN runs for a point */
    double interpolate_to,    /* I: current landsat pixel height */
    double *at_height         /* O: interpolated height for point */
)
{
    int parameter;
    int elevation;
    int below = 0;
    int above = 0;

    double below_parameters[AHP_NUM_PARAMETERS];
    double above_parameters[AHP_NUM_PARAMETERS];

    double slope;

    double above_height;
    double inv_height_diff; /* To remove the multiple divisions */

    /* Find the height to use that is below the interpolate_to height */
    for (elevation = 0; elevation < modtran_point.count; elevation++)
    {
        if (modtran_point.elevations[elevation].elevation < interpolate_to)
        {
            below = elevation; /* Last match will always be the one we want */
        }
    }

    /* Find the height to use that is equal to or above the interpolate_to
       height.  It will always be the same or the next height */ 
    above = below; /* Start with the same */
    if (above != (modtran_point.count - 1))
    {
        /* Not the last height */

        /* Check to make sure that we are not less that the below height,
           indicating that our interpolate_to height is below the first
           height */
        if (! (interpolate_to < modtran_point.elevations[above].elevation))
        {
            /* Use the next height, since it will be equal to or above our
               interpolate_to height */
            above++;
        }
        /* Else - We are at the first height, so use that for both above and
                  below */
    }
    /* Else - We are at the last height, so use that for both above and
              below */

    below_parameters[AHP_TRANSMISSION] =
        modtran_point.elevations[below].transmission;
    below_parameters[AHP_UPWELLED_RADIANCE] =
        modtran_point.elevations[below].upwelled_radiance;
    below_parameters[AHP_DOWNWELLED_RADIANCE] =
        modtran_point.elevations[below].downwelled_radiance;

    if (above == below)
    {
        /* Use the below parameters since the same */
        at_height[AHP_TRANSMISSION] =
            below_parameters[AHP_TRANSMISSION];
        at_height[AHP_UPWELLED_RADIANCE] =
            below_parameters[AHP_UPWELLED_RADIANCE];
        at_height[AHP_DOWNWELLED_RADIANCE] =
            below_parameters[AHP_DOWNWELLED_RADIANCE];
    }
    else
    {
        /* Interpolate between the heights for each parameter */
        above_height = modtran_point.elevations[above].elevation;
        inv_height_diff = 1.0 / (above_height
                                 - modtran_point.elevations[below].elevation);

        above_parameters[AHP_TRANSMISSION] =
            modtran_point.elevations[above].transmission;
        above_parameters[AHP_UPWELLED_RADIANCE] =
            modtran_point.elevations[above].upwelled_radiance;
        above_parameters[AHP_DOWNWELLED_RADIANCE] =
            modtran_point.elevations[above].downwelled_radiance;

        for (parameter = 0; parameter < AHP_NUM_PARAMETERS; parameter++)
        {
            slope = (above_parameters[parameter] - below_parameters[parameter])
                    * inv_height_diff;

            at_height[parameter] = slope*(interpolate_to - above_height)
                                 + above_parameters[parameter];
        }
    }
}


/******************************************************************************
METHOD:  interpolate_to_location

PURPOSE: Interpolate to location of current pixel
******************************************************************************/
static void interpolate_to_location
(
    GRID_POINTS *points,         /* I: The coordinate points */
    int *vertices,               /* I: The vertices for the points to use */
    double at_height[][AHP_NUM_PARAMETERS], /* I: current height atmospheric
                                                  results */
    double interpolate_easting,  /* I: interpolate to easting */
    double interpolate_northing, /* I: interpolate to northing */
    double *parameters           /* O: interpolated pixel atmospheric 
                                       parameters */
)
{
    int point;
    int parameter;

    double inv_h[NUM_CELL_POINTS];
    double w[NUM_CELL_POINTS];
    double total = 0.0;
    double inv_total;

    /* Shepard's method */
    for (point = 0; point < NUM_CELL_POINTS; point++)
    {
        inv_h[point] = 1.0 / sqrt (((points->points[vertices[point]].map_x
                                     - interpolate_easting)
                                    * (points->points[vertices[point]].map_x
                                       - interpolate_easting))
                                   +
                                   ((points->points[vertices[point]].map_y
                                     - interpolate_northing)
                                    * (points->points[vertices[point]].map_y
                                       - interpolate_northing)));

        total += inv_h[point];
    }

    /* Determine the weights for each vertex */
    inv_total = 1/total;
    for (point = 0; point < NUM_CELL_POINTS; point++)
    {
        w[point] = inv_h[point]*inv_total;
    }

    /* For each parameter apply each vertex's weighted value */
    for (parameter = 0; parameter < AHP_NUM_PARAMETERS; parameter++)
    {
        parameters[parameter] = 0.0;
        for (point = 0; point < NUM_CELL_POINTS; point++)
        {
            parameters[parameter] += (w[point] * at_height[point][parameter]);
        }
    }
}


/*****************************************************************************
METHOD:  determine_grid_point_distances

PURPOSE: Determines the distances for the current set of grid points.

NOTE: The indexes of the grid points are assumed to be populated.
*****************************************************************************/
static void determine_grid_point_distances
(
    GRID_POINTS *points,       /* I: All the available points */
    double longitude,          /* I: Longitude of the current line/sample */
    double latitude,           /* I: Latitude of the current line/sample */
    int num_grid_points,       /* I: The number of grid points to operate on */
    GRID_ITEM *grid_points     /* I/O: Sorted to determine the center grid
                                       point */
)
{
    int point;
    GRID_ITEM *pt = grid_points;  /* array pointer */

    /* Populate the distances to the grid points */
    for (point = 0; point < num_grid_points; point++, pt++)
    {
        pt->distance = haversine_distance(points->points[pt->index].lon,
                                          points->points[pt->index].lat,
                                          longitude, latitude);
    }
}


/*****************************************************************************
METHOD:  determine_center_grid_point

PURPOSE: Determines the index of the center point from the current set of grid
         points.

NOTE: The indexes of the grid points are assumed to be populated.

RETURN: type = int
    Value  Description
    -----  -------------------------------------------------------------------
    index  The index of the center point
*****************************************************************************/
static int determine_center_grid_point
(
    GRID_POINTS *points,       /* I: All the available points */
    double longitude,          /* I: Longitude of the current line/sample */
    double latitude,           /* I: Latitude of the current line/sample */
    int num_grid_points,       /* I: The number of grid points to operate on */
    GRID_ITEM *grid_points     /* I/O: Sorted to determine the center grid
                                       point */
)
{
    determine_grid_point_distances (points, longitude, latitude,
                                    num_grid_points, grid_points);

    /* Sort them to find the closest one */
    qsort (grid_points, num_grid_points, sizeof (GRID_ITEM),
           qsort_grid_compare_function);

    return grid_points[0].index;
}


/*****************************************************************************
METHOD:  determine_first_center_grid_point

PURPOSE: Determines the index of the first center point to use for the current
         line.  Only called when the fist valid point for a line is
         encountered.  The point is determined from all of the available
         points.

RETURN: type = int
    Value  Description
    -----  -------------------------------------------------------------------
    index  The index of the center point
*****************************************************************************/
static int determine_first_center_grid_point
(
    GRID_POINTS *points,       /* I: All the available points */
    double longitude,          /* I: Longitude of the current line/sample */
    double latitude,           /* I: Latitude of the current line/sample */
    GRID_ITEM *grid_points     /* I/O: Memory passed in, populated and
                                       sorted to determine the center grid
                                       point */
)
{
    int point;

    /* Assign the point indexes for all grid points */
    for (point = 0; point < points->count; point++)
    {
        grid_points[point].index = point;
    }

    return determine_center_grid_point (points, longitude, latitude,
                                        points->count, grid_points);
}


/*****************************************************************************
METHOD:  calculate_pixel_atmospheric_parameters

PURPOSE: Generate transmission, upwelled radiance, and downwelled radiance at
         each Landsat pixel

RETURN: SUCCESS
        FAILURE
*****************************************************************************/
static int calculate_pixel_atmospheric_parameters
(
    Input_Data_t *input,       /* I: input structure */
    GRID_POINTS *points,       /* I: The coordinate points */
    char *xml_filename,        /* I: XML filename */
    Espa_internal_meta_t xml_metadata, /* I: XML metadata */
    MODTRAN_POINTS *modtran_results /* I: results from MODTRAN runs */
)
{
    char FUNC_NAME[] = "calculate_pixel_atmospheric_parameters";

    int line;
    int sample;

    bool first_sample;

    double easting;
    double northing;

    Geoloc_t *space = NULL;    /* Geolocation information */
    Space_def_t space_def;     /* Space definition (projection values) */
    Img_coord_float_t img;     /* Floating point image coordinates */
    Geo_coord_t geo;           /* Geodetic coordinates */
    float longitude;           /* Longitude */
    float latitude;            /* Latitude */

    GRID_ITEM *grid_points = NULL;

    int vertex;
    int center_point;
    int cell_vertices[NUM_CELL_POINTS];

    double at_height[NUM_CELL_POINTS][AHP_NUM_PARAMETERS];
    double parameters[AHP_NUM_PARAMETERS];
    double avg_distance_ll;
    double avg_distance_ul;
    double avg_distance_ur;
    double avg_distance_lr;

    Intermediate_Data_t inter;

    int16_t *elevation_data = NULL; /* input elevation data in meters */

    double current_height;
    char msg[MAX_STR_LEN];

    /* Use local variables for cleaner code */
    int num_cols = points->cols;
    int num_points = points->count;
    int pixel_count = input->lines * input->samples;
    int pixel_loc;

    /* Open the intermedate data files */
    if (open_intermediate(input, &inter) != SUCCESS)
    {
        RETURN_ERROR("Opening intermediate data files", FUNC_NAME, FAILURE);
    }

    /* Allocate memory for the intermedate data */
    if (allocate_intermediate(&inter, pixel_count) != SUCCESS)
    {
        RETURN_ERROR("Allocating memory for intermediate data",
                     FUNC_NAME, FAILURE);
    }

    /* Allocate memory for elevation */
    elevation_data = malloc(pixel_count*sizeof(int16_t));
    if (elevation_data == NULL)
    {
        RETURN_ERROR("Allocating elevation_data memory", FUNC_NAME, FAILURE);
    }

    /* Allocate memory to hold the grid_points to the first sample of data for
       the current line */
    grid_points = malloc (num_points * sizeof (GRID_ITEM));
    if (grid_points == NULL)
    {
        RETURN_ERROR ("Allocating grid_points memory", FUNC_NAME, FAILURE);
    }

    /* Read thermal and elevation data into memory */
    if (read_input(input, inter.band_thermal, elevation_data, pixel_count)
        != SUCCESS)
    {
        RETURN_ERROR ("Reading thermal and elevation bands", FUNC_NAME,
                      FAILURE);
    }

    /* Get geolocation space definition */
    if (!get_geoloc_info(&xml_metadata, &space_def))
    {
        RETURN_ERROR ("Getting space metadata from XML file", FUNC_NAME,
                     FAILURE);
    }
    space = setup_mapping(&space_def);
    if (space == NULL)
    {
        RETURN_ERROR ("Setting up geolocation mapping", FUNC_NAME, FAILURE);
    }

    /* Show some status messages */
    LOG_MESSAGE("Iterate through all pixels in Landsat scene", FUNC_NAME);
    snprintf(msg, sizeof(msg), "Pixel Count = %d", pixel_count);
    LOG_MESSAGE(msg, FUNC_NAME);
    snprintf(msg,  sizeof(msg),"Lines = %d, Samples = %d",
             input->lines, input->samples);
    LOG_MESSAGE(msg, FUNC_NAME);

    /* Loop through each line in the image */
    for (line = 0, pixel_loc = 0; line < input->lines; line++)
    {
        /* Print status on every 1000 lines */
        if (!(line % 1000))
        {
            printf ("Processing line %d\n", line);
            fflush (stdout);
        }

        northing = input->meta.ul_map_corner.y - line*input->y_pixel_size;

        /* Set first_sample to be true */
        first_sample = true;
        for (sample = 0; sample < input->samples; sample++, pixel_loc++)
        {
            if (inter.band_thermal[pixel_loc] != ST_NO_DATA_VALUE)
            {
                /* Determine latitude and longitude for current line/sample */
                img.l = line;
                img.s = sample;
                img.is_fill = false;
                if (!from_space(space, &img, &geo))
                {
                    RETURN_ERROR ("Mapping from line/sample to longitude/"
                        "latitude", FUNC_NAME, FAILURE);
                }
                longitude = geo.lon * DEGREES_PER_RADIAN;
                latitude = geo.lat * DEGREES_PER_RADIAN;

                easting = input->meta.ul_map_corner.x
                    + (sample * input->x_pixel_size);
                if (first_sample)
                {
                    /* Determine the first center point from all of the
                       available points */
                    center_point = determine_first_center_grid_point(
                                       points, longitude, latitude,
                                       grid_points);

                    /* Set first_sample to be false */
                    first_sample = false;
                }
                else
                {
                    /* Determine the center point from the current 9 grid
                       points for the current line/sample */
                    center_point = determine_center_grid_point(
                                       points, longitude, latitude,
                                       NUM_GRID_POINTS, grid_points);
                }

                /* Fix the index values, since the points are from a new line
                   or were messed up during determining the center point */
                grid_points[CC_GRID_POINT].index = center_point;
                grid_points[LL_GRID_POINT].index = center_point - 1 - num_cols;
                grid_points[LC_GRID_POINT].index = center_point - 1;
                grid_points[UL_GRID_POINT].index = center_point - 1 + num_cols;
                grid_points[UC_GRID_POINT].index = center_point + num_cols;
                grid_points[UR_GRID_POINT].index = center_point + 1 + num_cols;
                grid_points[RC_GRID_POINT].index = center_point + 1;
                grid_points[LR_GRID_POINT].index = center_point + 1 - num_cols;
                grid_points[DC_GRID_POINT].index = center_point - num_cols;

                /* Fix the distances, since the points are from a new line or
                   were messed up during determining the center point */
                determine_grid_point_distances (points, longitude, latitude,
                                                NUM_GRID_POINTS, grid_points);

                /* Determine the average distances for each quadrant around
                   the center point. We only need to use the three outer grid 
                   points */
                avg_distance_ll = (grid_points[DC_GRID_POINT].distance
                                   + grid_points[LL_GRID_POINT].distance
                                   + grid_points[LC_GRID_POINT].distance)
                                  / 3.0;

                avg_distance_ul = (grid_points[LC_GRID_POINT].distance
                                   + grid_points[UL_GRID_POINT].distance
                                   + grid_points[UC_GRID_POINT].distance)
                                  / 3.0;

                avg_distance_ur = (grid_points[UC_GRID_POINT].distance
                                   + grid_points[UR_GRID_POINT].distance
                                   + grid_points[RC_GRID_POINT].distance)
                                  / 3.0;

                avg_distance_lr = (grid_points[RC_GRID_POINT].distance
                                   + grid_points[LR_GRID_POINT].distance
                                   + grid_points[DC_GRID_POINT].distance)
                                  / 3.0;

                /* Determine which quadrant is closer and setup the cell
                   vertices to interpolate over based on that */
                if (avg_distance_ll < avg_distance_ul
                    && avg_distance_ll < avg_distance_ur
                    && avg_distance_ll < avg_distance_lr)
                { /* LL Cell */
                    cell_vertices[LL_POINT] = center_point - 1 - num_cols;
                }
                else if (avg_distance_ul < avg_distance_ur
                    && avg_distance_ul < avg_distance_lr)
                { /* UL Cell */
                    cell_vertices[LL_POINT] = center_point - 1;
                }
                else if (avg_distance_ur < avg_distance_lr)
                { /* UR Cell */
                    cell_vertices[LL_POINT] = center_point;
                }
                else
                { /* LR Cell */
                    cell_vertices[LL_POINT] = center_point - num_cols;
                }

                /* UL Point */
                cell_vertices[UL_POINT] = cell_vertices[LL_POINT] + num_cols;
                /* UR Point */
                cell_vertices[UR_POINT] = cell_vertices[UL_POINT] + 1;
                /* LR Point */
                cell_vertices[LR_POINT] = cell_vertices[LL_POINT] + 1;

#if OUTPUT_CELL_DESIGNATION_BAND
                inter.band_cell[pixel_loc] = cell_vertices[LL_POINT];
#endif

                /* Convert height from m to km -- Same as 1.0 / 1000.0 */
                current_height = (double) elevation_data[pixel_loc] * 0.001;

                /* Interpolate three parameters to that height at each of the
                   four closest points */
                for (vertex = 0; vertex < NUM_CELL_POINTS; vertex++)
                {
                    int current_index = cell_vertices[vertex];

                    /* Interpolate three atmospheric parameters to current
                       height */
                    interpolate_to_height(
                        modtran_results->points[current_index],
                        current_height, at_height[vertex]);
                }

                /* Interpolate parameters at appropriate height to location of
                   current pixel */
                interpolate_to_location(points, cell_vertices, at_height,
                                        easting, northing, &parameters[0]);

                /* Convert radiances to W*m^(-2)*sr(-1) */
                inter.band_upwelled[pixel_loc] =
                    parameters[AHP_UPWELLED_RADIANCE] * 10000.0;
                inter.band_downwelled[pixel_loc] =
                    parameters[AHP_DOWNWELLED_RADIANCE] * 10000.0;
                inter.band_transmittance[pixel_loc] =
                    parameters[AHP_TRANSMISSION];
            } /* END - if not FILL */
            else
            {
                inter.band_upwelled[pixel_loc] = ST_NO_DATA_VALUE;
                inter.band_downwelled[pixel_loc] = ST_NO_DATA_VALUE;
                inter.band_transmittance[pixel_loc] = ST_NO_DATA_VALUE;

#if OUTPUT_CELL_DESIGNATION_BAND
                inter.band_cell[pixel_loc] = 0;
#endif
            }
        } /* END - for sample */

    } /* END - for line */

    /* Write out the temporary intermediate output files */
    if (write_intermediate(&inter, pixel_count) != SUCCESS)
    {
        sprintf (msg, "Writing to intermediate data files");
        RETURN_ERROR(msg, FUNC_NAME, FAILURE);
    }

    /* Free allocated memory */
    free(grid_points);
    free(elevation_data);
    free_intermediate(&inter);

    /* Close the intermediate binary files */
    if (close_intermediate(&inter) != SUCCESS)
    {
        sprintf (msg, "Closing file intermediate data files");
        RETURN_ERROR(msg, FUNC_NAME, FAILURE);
    }

    /* Add the ST intermediate bands to the metadata file */
    if (add_st_band_product(xml_filename,
                             input->reference_band_name,
                             inter.thermal_filename,
                             ST_THERMAL_RADIANCE_PRODUCT_NAME,
                             ST_THERMAL_RADIANCE_BAND_NAME,
                             ST_THERMAL_RADIANCE_SHORT_NAME,
                             ST_THERMAL_RADIANCE_LONG_NAME,
                             ST_RADIANCE_UNITS,
                             0.0, 0.0) != SUCCESS)
    {
        ERROR_MESSAGE ("Failed adding ST thermal radiance band product", 
            FUNC_NAME);
    }

    if (add_st_band_product(xml_filename,
                             input->reference_band_name,
                             inter.transmittance_filename,
                             ST_ATMOS_TRANS_PRODUCT_NAME,
                             ST_ATMOS_TRANS_BAND_NAME,
                             ST_ATMOS_TRANS_SHORT_NAME,
                             ST_ATMOS_TRANS_LONG_NAME,
                             ST_RADIANCE_UNITS,
                             0.0, 0.0) != SUCCESS)
    {
        ERROR_MESSAGE ("Failed adding ST atmospheric transmission band "
            "product", FUNC_NAME);
    }

    if (add_st_band_product(xml_filename,
                             input->reference_band_name,
                             inter.upwelled_filename,
                             ST_UPWELLED_RADIANCE_PRODUCT_NAME,
                             ST_UPWELLED_RADIANCE_BAND_NAME,
                             ST_UPWELLED_RADIANCE_SHORT_NAME,
                             ST_UPWELLED_RADIANCE_LONG_NAME,
                             ST_RADIANCE_UNITS,
                             0.0, 0.0) != SUCCESS)
    {
        ERROR_MESSAGE ("Failed adding ST upwelled radiance band product", 
            FUNC_NAME);
    }

    if (add_st_band_product(xml_filename,
                             input->reference_band_name,
                             inter.downwelled_filename,
                             ST_DOWNWELLED_RADIANCE_PRODUCT_NAME,
                             ST_DOWNWELLED_RADIANCE_BAND_NAME,
                             ST_DOWNWELLED_RADIANCE_SHORT_NAME,
                             ST_DOWNWELLED_RADIANCE_LONG_NAME,
                             ST_RADIANCE_UNITS,
                             0.0, 0.0) != SUCCESS)
    {
        ERROR_MESSAGE ("Failed adding ST downwelled radiance band product", 
            FUNC_NAME);
    }

    return SUCCESS;
}

/* Setup and cleanup functions */

/*****************************************************************************
Method:  load_grid_points_hdr

Description:  Loads the grid points header information.

Notes:
    1. The grid point header file must be present in the current working 
       directory.

RETURN: SUCCESS
        FAILURE
*****************************************************************************/
int load_grid_points_hdr
(
    GRID_POINTS *grid_points
)
{
    char FUNC_NAME[] = "load_grid_points_hdr";
    FILE *grid_fd = NULL;
    int status;
    char header_filename[] = "grid_points.hdr";
    char errmsg[PATH_MAX];

    /* Open the grid header file. */
    grid_fd = fopen(header_filename, "r");
    if (grid_fd == NULL)
    {
        snprintf(errmsg, sizeof(errmsg), "Failed opening %s", header_filename);
        RETURN_ERROR(errmsg, FUNC_NAME, FAILURE);
    }

    /* Read the grid header file. */
    errno = 0;
    status = fscanf(grid_fd, "%d\n%d\n%d", &grid_points->count,
                    &grid_points->rows, &grid_points->cols);
    if (status != 3 || errno != 0)
    {
        fclose(grid_fd);
        snprintf(errmsg, sizeof(errmsg), "Failed reading %s", header_filename);
        RETURN_ERROR(errmsg, FUNC_NAME, FAILURE);
    }

    fclose(grid_fd);

    return SUCCESS;
}


/*****************************************************************************
Method:  load_grid_points

Description:  Loads the grid points into a data structure.

Notes:
    1. The grid point files must be present in the current working directory.

RETURN: SUCCESS
        FAILURE
*****************************************************************************/
static int load_grid_points
(
    GRID_POINTS *grid_points
)
{
    char FUNC_NAME[] = "load_grid_points";

    FILE *grid_fd = NULL;

    int status;

    char binary_filename[] = "grid_points.bin";
    char errmsg[PATH_MAX];

    /* Initialize the points */
    grid_points->points = NULL;

    if (load_grid_points_hdr(grid_points) != SUCCESS)
    {
        RETURN_ERROR("Failed loading grid point header information",
                     FUNC_NAME, FAILURE);
    }

    grid_points->points = malloc(grid_points->count * sizeof(GRID_POINT));
    if (grid_points->points == NULL)
    {
        RETURN_ERROR("Failed allocating memory for grid points",
                     FUNC_NAME, FAILURE);
    }

    /* Open the grid point file */
    grid_fd = fopen(binary_filename, "rb");
    if (grid_fd == NULL)
    {
        snprintf(errmsg, sizeof(errmsg), "Failed opening %s", binary_filename);
        RETURN_ERROR(errmsg, FUNC_NAME, FAILURE);
    }

    /* Read the grid points */
    status = fread(grid_points->points, sizeof(GRID_POINT),
                   grid_points->count, grid_fd);
    if (status != grid_points->count || errno != 0)
    {
        fclose(grid_fd);
        snprintf(errmsg, sizeof(errmsg), "Failed reading %s", binary_filename);
        RETURN_ERROR(errmsg, FUNC_NAME, FAILURE);
    }

    fclose(grid_fd);

    return SUCCESS;
}


/*****************************************************************************
Method:  load_elevations

Description:  Loads the grid elevations into a data structure.

Notes:
    1. The grid elevation file must be present in the current working directory.
    2. The grid elevation entries should be in sync with the grid file.

RETURN: SUCCESS
        FAILURE
*****************************************************************************/
static int load_elevations
(
    MODTRAN_POINTS *modtran_points
)
{
    char FUNC_NAME[] = "load_elevations";

    FILE *elevation_fd = NULL;

    int status;
    int index;   /* Index into point structure */

    char elevation_filename[] = "grid_elevations.txt";
    char errmsg[PATH_MAX];

    snprintf(errmsg, sizeof(errmsg), "Failed reading %s", elevation_filename);

    elevation_fd = fopen(elevation_filename, "r");
    if (elevation_fd == NULL)
    {
        RETURN_ERROR(errmsg, FUNC_NAME, FAILURE);
    }

    /* Read the elevations into the 0 elevation positions in the MODTRAN 
       point structure.  The file and structure should have the same order. */
    for (index = 0; index < modtran_points->count; index++)
    {
        MODTRAN_POINT *modtran_ptr = &modtran_points->points[index];

        /* Keep looking for a modtran point that was actually run. */
        if (modtran_ptr->ran_modtran == 0)
        {
            continue; 
        }

        status = fscanf(elevation_fd, "%lf %lf\n", 
            &(modtran_ptr->elevations->elevation),
            &(modtran_ptr->elevations->elevation_directory));
        if (status <= 0)
        {
            RETURN_ERROR(errmsg, FUNC_NAME, FAILURE);
        }
    }

    fclose(elevation_fd);

    return SUCCESS;
}


/*****************************************************************************
Method:  free_grid_points

Description:  Free allocated memory for the grid points.
*****************************************************************************/
void free_grid_points
(
    GRID_POINTS *grid_points
)
{
    free(grid_points->points);
    grid_points->points = NULL;
}

/*****************************************************************************
Method:  free_modtran_points

Description:  Free allocated memory for the MODTRAN points.
*****************************************************************************/
void free_modtran_points
(
    MODTRAN_POINTS *modtran_points
)
{
    int index;     /* Index into MODTRAN points structure */

    for (index = 0; index < modtran_points->count; index++)
    {
        free(modtran_points->points[index].elevations);
        modtran_points->points[index].elevations = NULL;
    }

    free(modtran_points->points);
    modtran_points->points = NULL;
}


/*****************************************************************************
Method:  initialize_modtran_points

Description:  Allocate the memory need to hold the MODTRAN results and
              initialize known values.

RETURN: SUCCESS
        FAILURE
*****************************************************************************/
static int initialize_modtran_points
(
    GRID_POINTS *grid_points,      /* I: The coordinate points */
    MODTRAN_POINTS *modtran_points /* O: Memory Allocated */
)
{
    char FUNC_NAME[] = "initialize_modtran_points";

    double gndalt[MAX_NUM_ELEVATIONS];

    int index;
    int num_elevations;            /* Number of elevations actually used */
    int elevation_index;           /* Index into elevations */
    int status;                    /* Function return status */

    FILE *modtran_elevation_fd = NULL;

    char modtran_elevation_filename[] = "modtran_elevations.txt";
    char errmsg[PATH_MAX];

    snprintf(errmsg, sizeof(errmsg), "Failed reading %s", 
        modtran_elevation_filename);

    modtran_elevation_fd = fopen(modtran_elevation_filename, "r");
    if (modtran_elevation_fd == NULL)
    {
        RETURN_ERROR(errmsg, FUNC_NAME, FAILURE);
    }

    status = fscanf(modtran_elevation_fd, "%d\n", &num_elevations);
    if (status <= 0)
    {
        RETURN_ERROR(errmsg, FUNC_NAME, FAILURE);
    }

    /* Read the elevations into the gndalt structure. */ 
    for (index = 0; index < num_elevations; index++)
    {
        status = fscanf(modtran_elevation_fd, "%lf\n", &gndalt[index]);
        if (status <= 0)
        {
            RETURN_ERROR(errmsg, FUNC_NAME, FAILURE);
        }
    }

    fclose(modtran_elevation_fd);

    modtran_points->count = grid_points->count;

    modtran_points->points = malloc(modtran_points->count *
                                    sizeof(MODTRAN_POINT));
    if (modtran_points->points == NULL)
    {
        RETURN_ERROR("Failed allocating memory for modtran points",
                     FUNC_NAME, FAILURE);
    }

    for (index = 0; index < modtran_points->count; index++)
    {
        /* convenience pointers */
        MODTRAN_POINT *modtran_ptr = &modtran_points->points[index];
        GRID_POINT *grid_ptr = &grid_points->points[index];
        
        modtran_ptr->count = num_elevations;
        modtran_ptr->ran_modtran = grid_ptr->run_modtran;
        modtran_ptr->row = grid_ptr->row;
        modtran_ptr->col = grid_ptr->col;
        modtran_ptr->narr_row = grid_ptr->narr_row;
        modtran_ptr->narr_col = grid_ptr->narr_col;
        modtran_ptr->lon = grid_ptr->lon;
        modtran_ptr->lat = grid_ptr->lat;
        modtran_ptr->map_x = grid_ptr->map_x;
        modtran_ptr->map_y = grid_ptr->map_y;

        modtran_ptr->elevations = malloc(num_elevations*
                                         sizeof(MODTRAN_ELEVATION));
        if (modtran_ptr->elevations == NULL)
        {
            RETURN_ERROR("Failed allocating memory for modtran point"
                         " elevations", FUNC_NAME, FAILURE);
        }

        /* Iterate over the elevations and assign the elevation values. */
        for (elevation_index = 0; elevation_index < num_elevations; 
            elevation_index++)
        {
            MODTRAN_ELEVATION *mt_elev =
                                    &modtran_ptr->elevations[elevation_index];

            mt_elev->elevation = gndalt[elevation_index];
            mt_elev->elevation_directory = gndalt[elevation_index];
        }
    }

    /* Load the first elevation values if needed. */
    if (load_elevations(modtran_points) != SUCCESS)
    {
        RETURN_ERROR("calling load_elevations", FUNC_NAME, EXIT_FAILURE);
    }

    return SUCCESS;
}


/****************************************************************************
Method: usage

Description: Display help/usage information to the user.
****************************************************************************/
void usage()
{
    printf("Surface Temperature - st_atmospheric_parameters\n");
    printf("\n");
    printf("Generates interpolated atmospheric parameters covering the scene"
           " data.\n");
    printf("\n");
    printf("usage: st_atmospheric_parameters"
           " --xml=<filename>"
           " [--debug]\n");
    printf("\n");
    printf ("where the following parameters are required:\n");
    printf ("    --xml: name of the input XML file\n");
    printf ("\n");
    printf ("where the following parameters are optional:\n");
    printf ("    --debug: should debug output be generated?"
            " (default is false)\n");
    printf ("\n");
    printf ("st_atmospheric_parameters --help will print the ");
    printf ("usage statement\n");
    printf ("\n");
    printf ("Example: st_atmospheric_parameters"
            " --xml=LE07_L1T_028031_20041227_20160513_01_T1.xml\n");
    printf ("Note: This application must run from the directory"
            " where the input data is located.\n\n");
}


/*****************************************************************************
Method:  get_args

Description:  Gets the command-line arguments and validates that the required
              arguments were specified.

Returns: Type = int
    Value           Description
    -----           -----------
    FAILURE         Error getting the command-line arguments or a command-line
                    argument and associated value were not specified
    SUCCESS         No errors encountered

Notes:
    1. Memory is allocated for the input and output files.  All of these
       should be character pointers set to NULL on input.  The caller is
       responsible for freeing the allocated memory upon successful return.
*****************************************************************************/
static int get_args
(
    int argc,           /* I: number of cmd-line args */
    char *argv[],       /* I: string of cmd-line args */
    char *xml_filename, /* I: address of input XML metadata filename  */
    bool *debug         /* O: debug flag */
)
{
    int c;                         /* current argument index */
    int option_index;              /* index of the command line option */
    static int debug_flag = 0;     /* debug flag */
    char errmsg[MAX_STR_LEN];      /* error message */
    char FUNC_NAME[] = "get_args"; /* function name */

    static struct option long_options[] = {
        {"debug", no_argument, &debug_flag, 1},
        {"xml", required_argument, 0, 'i'},
        {"help", no_argument, 0, 'h'},
        {0, 0, 0, 0}
    };

    /* Loop through all the cmd-line options */
    opterr = 0; /* turn off getopt_long error msgs as we'll print our own */
    while (1)
    {
        /* optstring in call to getopt_long is empty since we will only
           support the long options */
        c = getopt_long(argc, argv, "", long_options, &option_index);
        if (c == -1)
        {
            /* Out of cmd-line options */
            break;
        }

        switch (c)
        {
            case 0:
                /* If this option set a flag, do nothing else now. */
                if (long_options[option_index].flag != 0)
                    break;

            case 'h':              /* help */
                usage ();
                return FAILURE;
                break;

            case 'i':              /* xml infile */
                snprintf(xml_filename, PATH_MAX, "%s", optarg);
                break;

            case '?':
            default:
                snprintf(errmsg, sizeof(errmsg),
                         "Unknown option %s", argv[optind - 1]);
                usage();
                RETURN_ERROR(errmsg, FUNC_NAME, FAILURE);
                break;
        }
    }

    /* Make sure the XML file was specified */
    if (strlen(xml_filename) <= 0)
    {
        usage();
        RETURN_ERROR("XML input file is a required argument", FUNC_NAME,
                     FAILURE);
    }

    /* Set the debug flag */
    if (debug_flag)
        *debug = true;
    else
        *debug = false;

    return SUCCESS;
}


/*****************************************************************************
Method:  main

Description:  Main for the application.
*****************************************************************************/
int main(int argc, char *argv[])
{
    char FUNC_NAME[] = "main";

    Espa_internal_meta_t xml_metadata;  /* XML metadata structure */
    char xml_filename[PATH_MAX];        /* Input XML filename */
    bool debug;                         /* Debug flag for debug output */
    Input_Data_t *input = NULL;         /* Input data and meta data */
    GRID_POINTS grid_points;            /* NARR grid points */
    MODTRAN_POINTS modtran_points;      /* Points that are processed through
                                           MODTRAN */

    /* Read the command-line arguments */
    if (get_args(argc, argv, xml_filename, &debug)
        != SUCCESS)
    {
        RETURN_ERROR("calling get_args", FUNC_NAME, EXIT_FAILURE);
    }

    /* Validate the input metadata file */
    if (validate_xml_file(xml_filename) != SUCCESS)
    {
        /* Error messages already written */
        return EXIT_FAILURE;
    }

    /* Initialize the metadata structure */
    init_metadata_struct(&xml_metadata);

    /* Parse the metadata file into our internal metadata structure; also
       allocates space as needed for various pointers in the global and band
       metadata */
    if (parse_metadata(xml_filename, &xml_metadata) != SUCCESS)
    {
        /* Error messages already written */
        return EXIT_FAILURE;
    }

    /* Open input file, read metadata, and set up buffers */
    input = open_input(&xml_metadata);
    if (input == NULL)
    {
        RETURN_ERROR("opening input files", FUNC_NAME, EXIT_FAILURE);
    }

    /* Load the grid points */
    if (load_grid_points(&grid_points) != SUCCESS)
    {
        RETURN_ERROR("calling load_grid_points", FUNC_NAME, EXIT_FAILURE);
    }

    /* Allocate and initialize the memory need to hold the MODTRAN results */
    if (initialize_modtran_points(&grid_points, &modtran_points) != SUCCESS)
    {
        RETURN_ERROR("calling initializing_modtran_points", FUNC_NAME, 
            EXIT_FAILURE);
    }

    /* Generate parameters for each height and NARR point */
    if (calculate_point_atmospheric_parameters(input, &grid_points, 
        &modtran_points) != SUCCESS)
    {
        RETURN_ERROR("calling calculate_point_atmospheric_parameters",
            FUNC_NAME, EXIT_FAILURE);
    }

    /* Process the grid points */
    printf("%d %d %d\n", grid_points.count, grid_points.rows, grid_points.cols);
    printf("%d %d %d\n", grid_points.points[0].index, grid_points.points[0].row,
        grid_points.points[0].col);
    printf("%d %d %d\n", grid_points.points[1].index, grid_points.points[1].row,
        grid_points.points[1].col);
    printf("%d %d %d\n", grid_points.points[2].index, grid_points.points[2].row,
        grid_points.points[2].col);
    printf("%d %d %d\n", grid_points.points[3].index, grid_points.points[3].row,
        grid_points.points[3].col);
    printf("%d %d %d\n", grid_points.points[15].index, 
       grid_points.points[15].row, grid_points.points[15].col);

    /* Using the values made at the grid points, generate atmospheric 
       parameters for each Landsat pixel */ 
    if (calculate_pixel_atmospheric_parameters(input, &grid_points, 
        xml_filename, xml_metadata, &modtran_points) != SUCCESS)
    {
        RETURN_ERROR("calling calculate_pixel_atmospheric_parameters", 
            FUNC_NAME, EXIT_FAILURE);
    }

    /* Free metadata */
    free_metadata(&xml_metadata);

    /* Free the grid and MODTRAN points */
    free_grid_points(&grid_points);
    free_modtran_points(&modtran_points);

    /* Close the input file and free the structure */
    close_input(input);

    return EXIT_SUCCESS;
}
