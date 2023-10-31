/* FileKit
 * Copyright (C) 2023 Jmcgee01125
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU Affero General Public License as published
 * by the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include <getopt.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#include "api.h"
#include "auth.h"
#include "http.h"
#include "server.h"

#define DEFAULT_PORT_STRING ("8012")
#define DEFAULT_FALLBACK_PATH ("200.html")
#define DEFAULT_FALLBACK_ROOT_PATH ("index.html")
#define DEFAULT_LOGIN_TOKEN_NAME ("filekit_auth")
#define DEFAULT_TOKEN_EXP_TIME (60 * 60)
#define DEFAULT_NUM_WORKERS (2)
#define DEFAULT_NUM_SLOTS (128)

static void usage(const char* prog_name)
{
    fprintf(stderr,
            "Usage: %s -w webdir -d datadir [-p port] [-f fallback] [-r root_fb] [-c name] [-e seconds] [-t threads] "
            "[-s slots] [-4] [-6] [-h]\n"
            "\t-w webdir     Root directory from which to serve frontend files.\n"
            "\t-d datadir    Root directory to hold data files.\n"
            "\t-p port       Port to run on (default: %s).\n"
            "\t-f fallback   Path to fallback resource (default: %s).\n"
            "\t-r root_fb    Path to fallback resource when the path is / (default: %s).\n"
            "\t-c name       Name of auth cookie (default: %s).\n"
            "\t-e seconds    Login token expiration time, in seconds (default: %d).\n"
            "\t-t threads    Number of worker threads to use for serving clients (default: %d).\n"
            "\t-s slots      Number of slots to use per worker thread (default: %d).\n"
            "\t-4            Bind IPv4 only.\n"
            "\t-6            Bind IPv6 only, or both when dual binding is enabled.\n"
            "\t-h            Show this help.\n",
            prog_name, DEFAULT_PORT_STRING, DEFAULT_FALLBACK_PATH, DEFAULT_FALLBACK_ROOT_PATH, DEFAULT_LOGIN_TOKEN_NAME,
            DEFAULT_TOKEN_EXP_TIME, DEFAULT_NUM_WORKERS, DEFAULT_NUM_SLOTS);
    exit(1);
}

int main(int argc, char* argv[])
{
    bool bind_ipv4 = true;
    bool bind_ipv6 = true;
    int num_workers = DEFAULT_NUM_WORKERS;
    int num_slots = DEFAULT_NUM_SLOTS;
    char* port_string = DEFAULT_PORT_STRING;
    char* fallback_path = DEFAULT_FALLBACK_PATH;
    char* root_fallback_path = DEFAULT_FALLBACK_ROOT_PATH;
    int auth_exp_time = DEFAULT_TOKEN_EXP_TIME;
    struct http_api_tree* http_api_tree = NULL;
    char* http_directory = NULL;
    char* filekit_directory = NULL;
    char* auth_cookie_name = DEFAULT_LOGIN_TOKEN_NAME;
    int opt;

    while ((opt = getopt(argc, argv, "w:d:p:f:r:c:e:t:s:46h")) != -1) {
        switch (opt) {
            case 'w':
                http_directory = optarg;
                break;
            case 'd':
                filekit_directory = optarg;
                break;
            case 'p':
                port_string = optarg;
                break;
            case 'f':
                fallback_path = optarg;
                break;
            case 'r':
                root_fallback_path = optarg;
                break;
            case 'c':
                auth_cookie_name = optarg;
                break;
            case 'e':
                auth_exp_time = atoi(optarg);
                break;
            case 't':
                num_workers = atoi(optarg);
                if (num_workers < 1) {
                    fprintf(stderr, "Invalid worker count (%d).\n", num_workers);
                    exit(1);
                }
                break;
            case 's':
                num_slots = atoi(optarg);
                if (num_slots < 1) {
                    fprintf(stderr, "Invalid slot count (%d).\n", num_slots);
                    exit(1);
                }
                break;
            case '4':
                bind_ipv4 = true;
                bind_ipv6 = false;
                break;
            case '6':
                bind_ipv4 = false;
                bind_ipv6 = true;
                break;
            case 'h':
            default:
                usage(argv[0]);
        }
    }

    if (!http_directory || !filekit_directory) {
        usage(argv[0]);
    }

    printf("Starting on port %s.\n", port_string);
    printf("Web root:  %s\n", http_directory);
    printf("Data root: %s\n", filekit_directory);

    http_api_tree = api_init(filekit_directory, auth_cookie_name, auth_exp_time);
    auth_init(auth_exp_time);
    http_init("FileKit", http_directory, fallback_path, root_fallback_path, true, auth_cookie_name, http_api_tree);
    start_server(port_string, bind_ipv4, bind_ipv6, num_workers, num_slots);

    return 0;
}
