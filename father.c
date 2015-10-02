#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <time.h>
#include <stdarg.h>

char cfgfile[256] = "/etc/father.conf";
char logfile[256] = "/var/log/father.log";
char pidfile[256] = "/var/run/father.pid";
char chrootbin[64] = "/usr/sbin/chroot";
int i_reset_interval = 120;
int i_check_interval = 5;
int i_max_failures = 10;
int i_failures_before_kill9 = 3;

int procs_count;
int logfile_h;

struct Process {
    char cmd[256];
    pid_t pid;
    int status;
    int fail;
    int kill;
} *procs = NULL;

void logger(char * format, ...)
{
    int file;
    char buff[1024];
    char tmp[32];
    va_list arg;
    time_t rawtime;
    struct tm *timeinfo;

    va_start(arg, format);

    time(&rawtime);
    timeinfo = localtime(&rawtime);
    sprintf(buff, "%d%02d%02d %02d%02d%02d father[%d]: ", (timeinfo->tm_year+1900), timeinfo->tm_mon+1, timeinfo->tm_mday, timeinfo->tm_hour, timeinfo->tm_min, timeinfo->tm_sec, (int) getpid());

    for (; *format; ++format) {
        switch (*format) {
            case 'i':
                sprintf(tmp, "%d", va_arg(arg, int));
                strcat(buff, tmp);
                break;
            case 's':
                strcat(buff, va_arg(arg, const char *));
                break;
            default:
                strcat(buff, "??TYPE??");
        }
    }

    va_end(arg);

    strcat(buff, "\0");
    write(logfile_h, buff, strlen(buff));
}

void procs_copy(struct Process *procs_new)
{
    int i;

    procs_new = (struct Process *) malloc (procs_count * sizeof(struct Process));

    for (i=0; i < procs_count; i++) {
        strcpy(procs_new[i].cmd, procs[i].cmd);
        procs_new[i].pid = (int) procs[i].pid;
        procs_new[i].status = procs[i].status;
        procs_new[i].fail = procs[i].fail;
        procs_new[i].kill = procs[i].kill;
    }
}

void sigint_handler (int sig)
{
    int i;
    FILE * file = NULL;

    switch(sig) {
        case 1:
            logger("s", "SIGHUP received, reloading config\n");
            if ((makeprocs()) == -1) {
                logger("sss", "Could not load config file (", cfgfile, ") !!! exiting\n");
                exit(-1);
            } else {
                logger("s", "Config reloaded\n");
            }
            break;

        case 2:
            logger("s", "Signal SIGINT has been handled!\n");
            break;

        case 10:
            logger("s", "Signal SIGUSR1 has been handled!\n");
            logger("s", "--------------------------------\n");
            logger("s", "writing procs structure content:\n");
            for (i=0; i<procs_count; i++) {
                logger("sisisisss", "PID: ", procs[i].pid, " FAIL: ", procs[i].fail, " KILL: ", procs[i].kill, " CMD: ", procs[i].cmd, "\n");
            }
            logger("s", "--------------------------------\n");
            break;

        case 15:
            logger("sis", "Signal SIGTERM has been handled, procs_count: ", procs_count, "\n");
            for (i=0; i<procs_count; i++) {
                logger("sis", "Sending kill to ", procs[i].pid, "\n");
                kill(procs[i].pid, SIGTERM);
            }
            if ((unlink(pidfile)) == -1) {
                logger("s", "cannot delete pidfile !\n");
            }
            logger("s", "father exited normally\n");
            exit(0);
            break;

        default:
            logger("sis", "received signal: ", sig, "\n");
    }
}

int makeprocs()
{
    int file;
    char buff[1024];
    char *tmp;
    char **filelines;
    size_t read_count;
    int lines_count = 0;
    int c = 0;
    int i, ok;

    struct Process *procs_tmp;
    struct Process *procs_new;

    if ((file = open(cfgfile, O_RDONLY)) == -1) {
        logger("sss", "can't open file: ", cfgfile, "\n");
    } else {
        logger("sss", "reading config: ", cfgfile, "\n");

        //read only 1024b of config file
        read_count = read(file, buff, 1024);
        buff[read_count] = '\0';
        close(read_count);

        logger("sisisisis", "c=", c, ", strlen(buff)=", strlen(buff), ",lines_count=", lines_count, ", read_count=", read_count, "\n");

        char config[32][256];
        lines_count = 0;
        tmp = strtok(buff, "\n");

        //count lines
        if (tmp[0] != '#') {
            strcpy(config[lines_count], tmp);
            logger("sisss", "cmd config ", lines_count, ": ", config[lines_count], "\n");
            lines_count++;
        }

        logger("sisisis", "c=", c, ", strlen(buff)=", strlen(buff), ",lines_count=", lines_count, "\n");

        //load config lines
        while (1) {
            if ((tmp = strtok(NULL, "\n")) == NULL) {
                break;
            }

            if (tmp[0] != '#') {
                strcpy(config[lines_count], tmp);
                logger("sisss", "cmd config ", lines_count, ": ", config[lines_count], "\n");
                lines_count++;
            }
        }

        logger("sisisis", "w c=", c, ", strlen(buff)=", strlen(buff), ",lines_count=", lines_count, "\n");

        //initialization
        if (procs == NULL) {
            logger("s", "making new procs\n");
            procs = (struct Process *) malloc (lines_count * sizeof(struct Process));
            procs_count = lines_count;

            //filling procs structure
            for (c=0; c<lines_count; c++) {
                strcpy(procs[c].cmd, config[c]);
                procs[c].pid = 0;
            }

            return lines_count;
        } else {
            logger("s", "altering procs\n");
            procs_tmp = (struct Process *) malloc (lines_count * sizeof(struct Process));

            for (c=0; c<lines_count; c++) {
                strcpy(procs_tmp[c].cmd,config[c]);
            }

            for (c=0; c<procs_count; c++)  {
                ok = 0;
                for (i=0; i<lines_count; i++) {
                    if ((strcmp(procs[c].cmd,procs_tmp[i].cmd)) == 0) {
                        procs_tmp[i].pid = (int) procs[c].pid;
                        procs_tmp[i].fail = procs[c].fail;
                        procs_tmp[i].kill = procs[c].kill;
                        ok = 1;
                        break;
                    } else {
                        continue;
                    }
                }

                // kills pids of deleted commands from config file
                if (ok == 0) {
                    logger ("sisss", "not usable: sending kill to: ", (int)procs[c].pid, ", cmd: ", procs[c].cmd, "\n");
                    kill (procs[c].pid,SIGTERM);
                    //waits until process ends
                    waitpid (procs[c].pid, &procs[c].status, 0);
                }
            }

            logger("s", "--------------------------------\n");
            logger("s", "writing procs_tmp structure content:\n");
            for (c=0; c<lines_count; c++) {
                logger("sisisisss", "PID: ", procs_tmp[c].pid, " FAIL: ", procs_tmp[c].fail, " KILL: ", procs_tmp[c].kill, " CMD: ", procs_tmp[c].cmd, "\n");
            }
            logger("s", "--------------------------------\n");

            // creating new proc structure
            logger("sis", "making new procs, lines_count=", lines_count, "\n");
            procs = (struct Process *) malloc (lines_count * sizeof(struct Process));
            for (i=0; i < lines_count; i++) {
                strcpy(procs[i].cmd, procs_tmp[i].cmd);
                procs[i].pid = (int) procs_tmp[i].pid;
                procs[i].status = procs_tmp[i].status;
                procs[i].fail = procs_tmp[i].fail;
                procs[i].kill = procs_tmp[i].kill;
            }

            procs_count = lines_count;
            logger("sis", "new procs_count=", procs_count, "\n");

            procs_tmp = NULL;
            procs_new = NULL;
        }
    }
}

void killall (const char *cmdorg, int signum)
{
    char cmd[256];
    char r_cmd[64];
    char **args;
    char *tmp = NULL;
    char tmp2[3];
    int c = 0;
    int words_count = 1;

    strcpy(cmd, cmdorg);

    if (tmp = strtok(cmd, " ")) {
        strcpy(cmd, tmp);
    }

    // looking for the last string - name of a binary
    tmp = strtok(cmd, "/");
    if (tmp == NULL) {
        strcpy (r_cmd, cmd);
    } else {
        while (tmp = strtok(NULL, "/")) {
            strcpy(r_cmd, tmp);
        }
    }

    logger("sisss", "killall -", signum, " ", r_cmd, "\n");
    strcpy(cmd, "killall -");
    sprintf(tmp2, "%d\0", signum);
    strcat(cmd, tmp2);
    strcat(cmd, " ");
    strcat(cmd, r_cmd);
    logger("ss", cmd, "\n");
    // sending a killall command
    system(cmd);
}


int spawn(const char *cmdorg)
{
    char cmd[256];
    char **args;
    char *tmp = NULL;
    int c = 0;
    int words_count = 1;
    int chrooted = 0;

    strcpy(cmd, cmdorg);

    while (c < strlen(cmd)) {
        if (cmd[c] == ' ') {
            words_count++;
        }
        c++;
    }

    // memory allocation for an array with arguments
    args = (char **) malloc (++words_count * sizeof(char*));

    for (c=0; c<words_count; c++) {
        args[c] = (char *) malloc (100 * sizeof(char));
    }

    // filling arguments array, first element = name of program
    tmp = strtok(cmd, " ");
    if (tmp == NULL) {
        strcpy(args[0], cmd);
    } else {
        strcpy(args[0], tmp);
    }

    // checking if program exists
    struct stat st;
    if (stat(args[0], &st) != 0) {
        logger("sssss", "spawn ", getpid(), ": ", args[0], ": no such file\n");
        exit(-1);
    }

    // if first argument is chroot binary
    if (strcmp(args[0], chrootbin) == 0) {
        chrooted = 1;
    }

    for (c=1; c<words_count-1; c++) {
        tmp = strtok(NULL, " ");
        strcpy(args[c], tmp);
    }

    // last argument has to be NULL
    args[c] = NULL;


    // if command has to be run in chroot enviroment
    if (chrooted) {
        if (chroot(args[1]) == -1) {
            logger("sss", "chroot to: ", args[1], " failed!\n");
            exit(1);
        } else {
            // after successful chroot the arguments array has to be rewriten
            for (c=0; c<words_count-3; c++) {
                strcpy(args[c], args[c+2]);
            }
            args[c] = NULL;
        }
    }

    // spawn a process
    if ((execve(args[0], args, NULL)) == -1) {
        perror("execve");
    }
}


int main()
{
    int c = 0;
    int lines_count;
    char pidbuff[6];
    int pid;
    int reset_counter = 0;
    int reset = 0;

    // check for pidfile
    if ((pid = open(pidfile, O_WRONLY | O_CREAT | O_EXCL, S_IWRITE | S_IREAD)) == -1) {
        logger("s", "father exited (pidfile exists)\n");
        printf("father exited (pidfile exists)\n");
        exit(0);
    } else {
        // create a pidfile
        sprintf(pidbuff, "%d\n", getpid());
        write(pid, pidbuff, strlen(pidbuff));
        close(pid);
    }

    if ((logfile_h = open(logfile, O_RDWR | O_CREAT | O_APPEND, S_IREAD | S_IWRITE)) == -1) {
        printf("could not load logfile\n");
        exit(0);
    }

    logger("s", "father started\n");

    // signal handling
    signal(SIGINT, sigint_handler);
    signal(SIGTERM, sigint_handler);
    signal(SIGHUP, sigint_handler);
    signal(SIGUSR1, sigint_handler);

    if ((procs_count = makeprocs()) == -1) {
        logger("sss", "Could not load config file (", cfgfile, ") !!! exiting\n");
        return -1;
    } else {

        //	    printf("%\n",procs_count);
        //	    logger("sis","before initial spawn, procs_count=",procs_count,"\n");
        //	    for (c=0;c<procs_count; c++)
        //	    {
        //		    logger("sisisisss","PID: ",procs[c].pid," FAIL: ",procs[c].fail," KILL: ",procs[c].kill," CMD: ",procs[c].cmd,"\n");
        //	    }
        //	    logger("s","--------------------------------\n");

        // initial spawning
        for (c=0; c<procs_count; c++) {
            switch(procs[c].pid = fork()) {
                case -1:
                    perror("fork");
                    logger("sss", "could'n spawn: ", procs[c].cmd, " fork failed !!!\n");
                    //		    exit(1); //father dead!
                break;

                case 0:
                    logger("sss", "spawned: ", procs[c].cmd, "\n");
                    spawn(procs[c].cmd);
                //		    break;
                //		default: break;
            }
        }

        logger("sis", "spawned ", c, "\n");

        // childs checking
        while(1) {
            sleep(i_check_interval);
            reset_counter += i_check_interval;

            if ((reset_counter >= i_reset_interval) && (reset)) {
                logger("s", "father: resetting failures\n");
                for (c=0; c<procs_count; c++) {
                    procs[c].fail=0;
                }
                reset_counter = 0;
                reset = 0;
            }

            for (c=0; c<procs_count; c++) {
                if (procs[c].pid == 0) {
                    // spawn a new process
                    switch(procs[c].pid = fork()) {
                        case -1:
                            perror("fork");
                            logger("sss", "father: could'n spawn: ", procs[c].cmd, " fork failed !!!\n");
                            //				exit(1); //father dead!
                        break;

                        case 0:
                            logger("sss", "spawned: ", procs[c].cmd, "\n");
                            spawn(procs[c].cmd);
                            //				break;
                            //				default: break;
                    }
                }

                // checking for processes
                if (waitpid(procs[c].pid, &procs[c].status, WNOHANG)) {
                    // process got signal 15 i waits for fail reset
                    if (procs[c].fail < 0) {
                        // if process reaches a fail limit (i_max_failures) it will receive -9 signal next time
                        procs[c].kill = 1;
                        continue;
                    }

                    // sending kill -9 signal
                    if (procs[c].kill == 1) {
                        procs[c].kill = 0;
                        killall(procs[c].cmd, 9);
                    }

                    if (procs[c].fail >= i_max_failures) {
                        logger("sss","\"", procs[c].cmd, "\" too many failures !!!\n");
                        if (procs[c].kill == 1) {
                            procs[c].fail = -1;
                            procs[c].kill = 0;
                            killall(procs[c].cmd, 9);
                        } else {
                            procs[c].fail = i_max_failures - i_failures_before_kill9;
                            procs[c].kill = 1;
                            reset = 1;
                            killall(procs[c].cmd, 15);
                        }
                    } else {
                        procs[c].fail++;

                        if (!WIFEXITED(procs[c].status)) {
                            logger("sssis", "\"", procs[c].cmd, "\" exit with error status: ", WEXITSTATUS(procs[c].status), "\n");
                        } else {
                            logger("sss", "\"", procs[c].cmd, "\" exited normally\n");
                        }
                        if (WIFSIGNALED(procs[c].status)) {
                            logger("sssis", "\"", procs[c].cmd, "\" exit because of signal: ", WTERMSIG(procs[c].status), "\n");
                        }

                        switch(procs[c].pid = fork()) {
                            case -1:
                                perror("fork");
                                logger("sss", "could'n respawn: \"", procs[c].cmd, "\" fork failed !!!\n");
                            case 0:
                                logger("sisss", "respawned(", procs[c].fail, "): \"", procs[c].cmd, "\"\n");
                                spawn(procs[c].cmd);
                        }
                    }
                }
            }
        }
    }
}
