/*******************************************************************************
#                                                                              #
#      MJPG-streamer allows to stream JPG frames from an input-plugin          #
#      to several output plugins                                               #
#                                                                              #
#      Copyright (C) 2007 Tom Stöveken                                         #
#                                                                              #
# This program is free software; you can redistribute it and/or modify         #
# it under the terms of the GNU General Public License as published by         #
# the Free Software Foundation; version 2 of the License.                      #
#                                                                              #
# This program is distributed in the hope that it will be useful,              #
# but WITHOUT ANY WARRANTY; without even the implied warranty of               #
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the                #
# GNU General Public License for more details.                                 #
#                                                                              #
# You should have received a copy of the GNU General Public License            #
# along with this program; if not, write to the Free Software                  #
# Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA    #
#                                                                              #
*******************************************************************************/

/*
  This output plugin is based on code from output_file.c
  Writen by Sebastian Muszytowski
  Version 0.1, September 2018

  Provides means to store data to mjpeg files in a ringbuffer style way.
*/
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/statvfs.h>
#include <errno.h>
#include <signal.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <getopt.h>
#include <pthread.h>
#include <fcntl.h>
#include <time.h>
#include <syslog.h>
#include <dirent.h>

#include "output_mjpeg.h"

#include "../../utils.h"
#include "../../mjpg_streamer.h"

#define OUTPUT_PLUGIN_NAME "MJPEG output plugin"

static pthread_t worker;
static globals *pglobal;
static int fd, usage_percentage, max_frame_size;
static char *folder = "/tmp";
static unsigned char *frame = NULL;
static int input_number = 0;
static char *file_name = NULL;

/******************************************************************************
Description.: print a help message
Input Value.: -
Return Value: -
******************************************************************************/
void help(void)
{
    fprintf(stderr, " ---------------------------------------------------------------\n" \
            " Help for output plugin..: "OUTPUT_PLUGIN_NAME"\n" \
            " ---------------------------------------------------------------\n" \
            " The following parameters can be passed to this plugin:\n\n" \
            " [-f | --folder ]........: folder to save pictures\n" \
            " [-s | --size ]..........: percentage of FS usage when old file is deleted\n" \
            " ---------------------------------------------------------------\n");
}

/******************************************************************************
Description.: get next or oldest file number
Input Value.: folder, type (see header file for types)
Return Value: int with next file number
******************************************************************************/
int get_file_number(char *folder, int type){
    int i = (type == NEW_FILE) ? 0 : 999999;
    struct dirent *de;
    DIR *dr = opendir(folder);

    if (dr == NULL) // opendir returns NULL if couldn't open directory
    {
        fprintf(stderr,"Could not open current directory");
        return 0;
    }

    while ((de = readdir(dr)) != NULL){
        int j;
        DBG("Scanned file %s\n", de->d_name);
        if(sscanf(de->d_name, FILE_PREFIX"%d[^.]",j) == 1){
            if (type == NEW_FILE) {
                if (j > i) { i = j+1; }
            } else if (type == OLDEST_FILE) {
                if (j < i) { i = j; }
            }
        }
    }
       
    closedir(dr);
    return i;
}

/******************************************************************************
Description.: Get disk space
Input Value.: -
Return Value: double remaining space percent
******************************************************************************/
double get_disk_percentage_free(){
    struct statvfs buffer;
    int ret = statvfs(folder, &buffer);

    if (ret == 0){
        const double total = (double)(buffer.f_blocks * buffer.f_frsize);
        const double available = (double)(buffer.f_bfree * buffer.f_frsize);
        const double used = total - available;
        return (double)(used / total) * (double)100;
    }
    
    return (double)100;
}
/******************************************************************************
Description.: Opens file
Input Value.: -
Return Value: status code
******************************************************************************/
int open_file(){
    int fn_number = get_file_number(folder, NEW_FILE);
    // create filename
    if (file_name != NULL) {
        free(file_name);
    }
    file_name = malloc(snprintf(0, 0, FILE_PREFIX "%06d" FILE_EXTENSION, fn_number) + 1);
    sprintf(file_name, FILE_PREFIX "%06d" FILE_EXTENSION, fn_number);
    // create file path
    char *fnBuffer = malloc(strlen(file_name) + strlen(folder) + 3);
    sprintf(fnBuffer, "%s/%s", folder, file_name);

    if ((fd = open(fnBuffer, O_CREAT | O_RDWR | O_TRUNC, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH)) < 0)
    {
        OPRINT("could not open the file %s\n", fnBuffer);
        free(fnBuffer);
        return 1;
    }
    free(fnBuffer);

    return 0;
}

/******************************************************************************
Description.: Deletes oldest file
Input Value.: -
Return Value: status code
******************************************************************************/
int delete_oldest_file(){
    // get oldest file
    int fn_number = get_file_number(folder, OLDEST_FILE);
    char *del_file_name = malloc(snprintf(0, 0, FILE_PREFIX "%06d" FILE_EXTENSION, fn_number) + 1);
    sprintf(del_file_name, FILE_PREFIX "%06d" FILE_EXTENSION, fn_number);
    // create file path
    char *fnBuffer = malloc(strlen(file_name) + strlen(folder) + 3);
    sprintf(fnBuffer, "%s/%s", folder, file_name);

    if(remove(fnBuffer) != 0){
        OPRINT("could not remove file %s\n", fnBuffer);
        free(fnBuffer);
        free(del_file_name);
        return 1;
    }
    OPRINT("Removed file %s\n", fnBuffer);
    free(fnBuffer);
    free(del_file_name);
    return 0;
}

/******************************************************************************
Description.: Get current file disk usage in percent
Input Value.: -
Return Value: double file usage in percent
******************************************************************************/
double get_current_file_percentage(){
    struct stat st;
    struct statvfs stfs;
    if (statvfs(folder, &stfs) == 0 && fstat(fd, &st) ==0){
        const double total = (double)(stfs.f_blocks * stfs.f_frsize);
        const double fsize = (double)st.st_size;
        return (double)(fsize / total) * (double)100;
    }
    return 0;
}

/******************************************************************************
Description.: clean up allocated resources
Input Value.: unused argument
Return Value: -
******************************************************************************/
void worker_cleanup(void *arg)
{
    static unsigned char first_run = 1;

    if (file_name != NULL) {
        close(fd);
    }

    if(!first_run) {
        DBG("already cleaned up resources\n");
        return;
    }

    first_run = 0;
    OPRINT("cleaning up resources allocated by worker thread\n");

    if(frame != NULL) {
        free(frame);
    }
    close(fd);
}


/******************************************************************************
Description.: this is the main worker thread
              it loops forever, grabs a fresh frame and stores it to file
Input Value.:
Return Value:
******************************************************************************/
void *worker_thread(void *arg)
{
    int ok = 1, frame_size = 0, rc = 0;
    char buffer1[1024] = {0}, buffer2[1024] = {0};
    unsigned long long counter = 0;
    time_t t;
    unsigned char *tmp_framebuffer = NULL;

    /* set cleanup handler to cleanup allocated resources */
    pthread_cleanup_push(worker_cleanup, NULL);

    while(ok >= 0 && !pglobal->stop) {
        DBG("waiting for fresh frame\n");

        pthread_mutex_lock(&pglobal->in[input_number].db);
        pthread_cond_wait(&pglobal->in[input_number].db_update, &pglobal->in[input_number].db);

        /* read buffer */
        frame_size = pglobal->in[input_number].size;

        /* check if buffer for frame is large enough, increase it if necessary */
        if(frame_size > max_frame_size) {
            DBG("increasing buffer size to %d\n", frame_size);

            max_frame_size = frame_size + (1 << 16);
            if((tmp_framebuffer = realloc(frame, max_frame_size)) == NULL) {
                pthread_mutex_unlock(&pglobal->in[input_number].db);
                LOG("not enough memory\n");
                return NULL;
            }

            frame = tmp_framebuffer;
        }

        /* copy frame to our local buffer now */
        memcpy(frame, pglobal->in[input_number].buf, frame_size);

        /* allow others to access the global buffer again */
        pthread_mutex_unlock(&pglobal->in[input_number].db);

        if(write(fd, frame, frame_size) < 0) {
                OPRINT("could not write to file %s",file_name);
                perror("write()");
                close(fd);
                return NULL;
        }

        counter++;
        /* typical frame rate, so once per second */
        if(counter > 30){
            if(get_current_file_percentage() > (double)FILE_PERCENTAGE_THRESHOLD){
                close(fd);
                if(open_file() == 1){
                    perror("Could not open new file!");
                    return NULL;
                }
                OPRINT("Opened new output file: %s\n", file_name)
            }
            if(get_disk_percentage_free() < (double)usage_percentage){
                delete_oldest_file();
            }
            counter = 0;
        }
    }

    /* cleanup now */
    pthread_cleanup_pop(1);

    return NULL;
}

/*** plugin interface functions ***/
/******************************************************************************
Description.: this function is called first, in order to initialize
              this plugin and pass a parameter string
Input Value.: parameters
Return Value: 0 if everything is OK, non-zero otherwise
******************************************************************************/
int output_init(output_parameter *param, int id)
{
	int i;
    pglobal = param->global;
    pglobal->out[id].name = malloc((1+strlen(OUTPUT_PLUGIN_NAME))*sizeof(char));
    sprintf(pglobal->out[id].name, "%s", OUTPUT_PLUGIN_NAME);
    DBG("OUT plugin %d name: %s\n", id, pglobal->out[id].name);

    param->argv[0] = OUTPUT_PLUGIN_NAME;

    /* show all parameters for DBG purposes */
    for(i = 0; i < param->argc; i++) {
        DBG("argv[%d]=%s\n", i, param->argv[i]);
    }

    reset_getopt();
    while(1) {
        int option_index = 0, c = 0;
        static struct option long_options[] = {
            {"h", no_argument, 0, 0},
            {"help", no_argument, 0, 0},
            {"f", required_argument, 0, 0},
            {"folder", required_argument, 0, 0},
            {"s", required_argument, 0, 0},
            {"size", required_argument, 0, 0},
            {0, 0, 0, 0}
        };

        c = getopt_long_only(param->argc, param->argv, "", long_options, &option_index);

        /* no more options to parse */
        if(c == -1) break;

        /* unrecognized option */
        if(c == '?') {
            help();
            return 1;
        }

        switch(option_index) {
            /* h, help */
        case 0:
        case 1:
            DBG("case 0,1\n");
            help();
            return 1;
            break;

            /* f, folder */
        case 2:
        case 3:
            DBG("case 2,3\n");
            folder = malloc(strlen(optarg) + 1);
            strcpy(folder, optarg);
            if(folder[strlen(folder)-1] == '/')
                folder[strlen(folder)-1] = '\0';
            break;

            /* s, size */
        case 4:
        case 5:
            DBG("case 4,5\n");
            usage_percentage = atoi(optarg);
            if(usage_percentage < 0 || usage_percentage > 99){
                OPRINT("ERROR: size must be between 0 and 99");
                return -1;
            }
            break;
        }

    }

    if(!(input_number < pglobal->incnt)) {
        OPRINT("ERROR: the %d input_plugin number is too much only %d plugins loaded\n", input_number, param->global->incnt);
        return 1;
    }

    OPRINT("output folder.....: %s\n", folder);
    OPRINT("input plugin.....: %d: %s\n", input_number, pglobal->in[input_number].plugin);

    int ret = open_file();
    OPRINT("output file.......: %s\n", file_name);

    return ret;
}

/******************************************************************************
Description.: calling this function stops the worker thread
Input Value.: -
Return Value: always 0
******************************************************************************/
int output_stop(int id)
{
    DBG("will cancel worker thread\n");
    pthread_cancel(worker);
    return 0;
}

/******************************************************************************
Description.: calling this function creates and starts the worker thread
Input Value.: -
Return Value: always 0
******************************************************************************/
int output_run(int id)
{
    DBG("launching worker thread\n");
    pthread_create(&worker, 0, worker_thread, NULL);
    pthread_detach(worker);
    return 0;
}