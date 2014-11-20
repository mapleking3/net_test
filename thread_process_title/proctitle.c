/** 
 * @file:       proctitle.c
 * @brief:      Implementation of an API to set process title
 * @author:     modify from openvas source code
 * @date:       2014-11-20
 * @version:    V1.0.0
 * @note:       History:
 * @note:       <author><time><version><description>
 * @warning:    
 */
#include "proctitle.h"

#include <string.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

static int argv_len;
static char **old_argv;
extern char **environ;

/**
 * @brief Initializes the process setting variables.
 *
 * @param[in]   argc    Argc argument from main.
 * @param[in]   argv    Argv argument from main.
 */
void
proctitle_init (int argc, char **argv)
{
    int i = 0;
    char **envp = environ;

    if (argv == NULL)
        return;
    /* Move environ to new memory, to be able to reuse older one. */
    while (envp[i]) i++;
    environ = calloc(1, sizeof(char *) * (i + 1));
    if (NULL == environ) {
        printf("calloc error!\n");
        exit(-1);
    }
    for (i = 0; envp[i]; i++)
    {
        environ[i] = calloc(1, strlen(envp[i])+1);
        if (NULL == environ) {
            printf("calloc error!\n");
            exit(-1);
        }
        strcpy(environ[i], envp[i]);
    }
    environ[i] = NULL;

    old_argv = argv;
    if (i > 0)
        argv_len = envp[i-1] + strlen(envp[i-1]) - old_argv[0];
    else
        argv_len = old_argv[argc-1] + strlen(old_argv[argc-1]) - old_argv[0];
}

/**
 * @brief Sets the process' title.
 *
 * @param[in]   new_title   Format string for new process title.
 * @param[in]   args        Format string arguments variable list.
 */
static void
proctitle_set_args (const char *new_title, va_list args)
{
  int i;
  char formatted[1024] = {0};

  if (old_argv == NULL)
    /* Called setproctitle before initproctitle ? */
    return;

  vsnprintf(formatted, sizeof(formatted), new_title, args);

  i = strlen (formatted);
  if (i > argv_len - 2)
    {
      i = argv_len - 2;
      formatted[i] = '\0';
    }
  bzero (old_argv[0], argv_len);
  strcpy (old_argv[0], formatted);
  old_argv[1] = NULL;
}

/**
 * @brief Sets the process' title.
 *
 * @param[in]   new_title   Format string for new process title.
 * @param[in]   ...         Arguments for format string.
 */
void
proctitle_set (const char *new_title, ...)
{
  va_list args;

  va_start (args, new_title);
  proctitle_set_args (new_title, args);
  va_end (args);
}
