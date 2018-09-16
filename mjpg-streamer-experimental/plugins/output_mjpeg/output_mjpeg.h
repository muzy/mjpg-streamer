#ifndef OUTPUT_MJPEG_H
#define OUTPUT_MJPEG_H

#define FILE_PREFIX "REC"
#define FILE_EXTENSION ".mjpeg"

#define MJPEG_HEADER "\r\n--myboundary\r\nContent-Type:image/jpeg\r\nContent-Length:%d\r\n\r\n"

#define OLDEST_FILE 1
#define NEW_FILE 2

#define FILE_PERCENTAGE_THRESHOLD 25

#endif
