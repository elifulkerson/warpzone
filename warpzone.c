/*

Welcome to Warp Zone!

You are in bash, you launch warpzone.  Warpzone launches a new bash, appending a green | to your prompt so you know that it is there.
I just passes your input through for the most part.
Unless you do ALT-\ or ALT-|  (yeah its technically alt-\ instead of pipe because I didn't want to force alt-shift-\)
If you do that, a green | appears.  You are now putting input into warpzone rather than the bottom bash.

Once you hit enter, warpzone sends the text to the *left* of the green | to the slave bash, grabs and holds the returned text
in a temporary file, then runs that temporary file as input into the pipeline specified to the *right* of the green |, then
drops the output of that pipeline back into your stdout.

This has the effect of lettings you use text utilities from the local system on output from the remote system, inline.


CONSIDER.  Instead of switching back to cooked input mode after a green pipe, do something so you can catch backspace and un-green-pipe your life?
           @@Need some way to un green pipe regardless.

ADD:  command flags.  --dont-change-prompt, --launch-this-shell


BIG CAVEAT:  I don't have nearly a gray enough beard, I don't fully understand all the subtleties of passing data back and forth on the TTY.  This program
works for my primary intended use case (using native unix utilities while being inside of for instance a Cisco ssh session) but it has a lot of distance to
cover before it could be considered a general use tool.


BUG:  Arrow keys work fine bash->bash, but not bash->csh.  Why?
FIXED? BUG:  Job control isn't working in the PTY
BUG:  For instance, terminal size negotiation (in particular, ssh to another server and run ssh) isn't working properly.
HALF FIXED? BUG:  Firing up stuff that does fancy curses (in particular, emacs) from within warpzone crashes.  Update - emacs works now, but tty size isn't set correctly.


// This program is originally based on a PTY example provided by R. Koucha :   http://rachid.koucha.free.fr/tech_corner/pty_pdip.html


warpzone
v0.2 Dec 28 2018

Eli Fulkerson
https://www.elifulkerson.com

*/

#define _XOPEN_SOURCE 600 
#include <stdlib.h> 
#include <fcntl.h> 
#include <errno.h> 
#include <unistd.h> 
#include <stdio.h> 
#define __USE_BSD 
#include <termios.h> 

#include <sys/select.h>

#include <sys/time.h>
#include <string.h>

struct termios oldtermios;
int global_ignore_child_handler = 0;


void handler(int sig) {

    if (global_ignore_child_handler == 1) {
      return;
    }
    
    printf("Signal %d\n", sig);

    // Reset the terminal so all our crap doesn't get out...
    tcsetattr(fileno(stdin), TCSAFLUSH, &oldtermios);
	
    exit(0);
}

void printbar (int length) {

    // ▄ █ ▀
	
	int sofar = 0;
	
	while (length > sofar) {

		int color = (rand() % 7) + 31;
		int block = (rand() % 9);

		switch(block) {
		case 0:
			printf("\033[%dm", color);
			printf("▀▀█ ");
			printf("\033[39m");
			sofar += 4;
			break;
		case 1:
			printf("\033[%dm", color);
			printf("██ ");
			printf("\033[39m");
			sofar += 3;
			break;
		case 2:
			printf("\033[%dm", color);
			printf("▄▄▄▄ ");
			printf("\033[39m");
			sofar += 5;
			break;
		case 3:
			printf("\033[%dm", color);
			printf("▀▀▀▀ ");
			printf("\033[39m");
			sofar += 5;
			break;
		case 4:
			printf("\033[%dm", color);
			printf("▄▄█ ");
			printf("\033[39m");
			sofar += 4;
			break;
		case 6:
			printf("\033[%dm", color);
			printf("█▀▀ ");
			printf("\033[39m");
			sofar += 4;
			break;
		case 7:
			printf("\033[%dm", color);
			printf("█▄▄ ");
			printf("\033[39m");
			sofar += 4;
			break;
		case 8:
			printf("\033[%dm", color);		 
			printf("▄█▀ ");
			printf("\033[39m");
			sofar += 4;
			break;
		case 9:
			printf("\033[%dm", color);
			printf("▀█▄ ");
			printf("\033[39m");
			sofar += 4;
			break;
		default:			
			break;
		}
		

	}
	printf("\n");	
}

int main(void) 
{ 

	//system("stty -a");

  int fdm, fds, rc; 
  char input[150];

  char inputchar[1];
  char previnputchar[1];
  char prevprevinputchar[1];
  
  //char save_previnputchar[1]; // lol... sigh


  char pipe_argument[1024];
  char pipe_slave_output[102400];

  int debug_inputstream = 0;

  // we want to exit if we lose our child, otherwise you get stuck in Limbo
  signal(SIGCHLD, handler);

  // save the original terminal settings to reset on signal to clean up
  rc = tcgetattr(fileno(stdin), &oldtermios);

  fdm = posix_openpt(O_RDWR); 
  
  if (fdm < 0) 
  { 
    fprintf(stderr, "Error %d on posix_openpt()\n", errno); 
    return 1; 
  } 

  rc = grantpt(fdm); 
  if (rc != 0) 
  { 
    fprintf(stderr, "Error %d on grantpt()\n", errno); 
    return 1; 
  } 

  rc = unlockpt(fdm); 
  if (rc != 0) 
  { 
    fprintf(stderr, "Error %d on unlockpt()\n", errno); 
    return 1; 
  } 


  // Open the slave PTY
  fds = open(ptsname(fdm), O_RDWR); 

  // Creation of a child process
  if (fork()) 
  { 
    // parent
    
    struct termios parent_orig_term_settings; // Saved terminal settings 
    struct termios new_term_settings; // Current terminal settings 

    // Save the default parameters of the parent side of the PTY 
    rc = tcgetattr(fds, &parent_orig_term_settings); 

    // Set raw mode on the parent side of the PTY
    new_term_settings = parent_orig_term_settings; 
    cfmakeraw (&new_term_settings); 
    tcsetattr (fileno(stdin), TCSANOW, &new_term_settings); 

    // this is the timeout for the select reading the slave side of the PTY
    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = 10;

    // Close the slave side of the PTY 
    close(fds); 

    // are we *READY* to begin reading input as of the next enter-keystroke-sent-to-slave?
    int in_magic_pipe_ready_mode = 0;

    // we are currently grabbing all of slave's input for the magic pipe
    int in_magic_pipe_reading_mode = 0;

    // Using a timeval for output grabbing from the slave terminal
    struct timeval t1;
    struct timeval t2;

    // Temporary name and file are used by the |
    char tmp_name[L_tmpnam];
    FILE* tmp_file;

    // This is sloppy, but I want a signifier that I am inside of warpzone, so we're appending a green ^ to the prompt... by typing it in
    sprintf(input, "PS1+=\"\\e[0;32m\\]|\\[\\e[0;40m\\]\"; # adding | to prompt\n\0");
    write(fdm, input,54);

    while (1) 
    { 

      fcntl (0, F_SETFL, O_NONBLOCK);

	  prevprevinputchar[0] = previnputchar[0];
      previnputchar[0] = inputchar[0];
	  rc = read(fileno(stdin), inputchar, sizeof(inputchar) );

	  if (debug_inputstream == 1) {
		  printf("%d ", inputchar[0], inputchar[0]);
		  fflush(stdout);
	  }

      // ok, so handling should go as follows.
      // if its an ALT (an escape, 27) it should be HELD until the next cycle.
      // if the NEXT KEY after alt is | or \, we go into pipe mode.  if not,
      // we clear the escape by sending the escape down the wire, then our current input.

	  
      // wherein we just eat alt and deal with it later inside of previnputchar
      if (inputchar[0] == 27) {
        continue;
      }

	  // if we're ready and we're in the mode and our last three characters are SPACE ALT \ ...
      if (in_magic_pipe_ready_mode == 0 && in_magic_pipe_reading_mode == 0 && previnputchar[0] == 27 && inputchar[0] == 92 && prevprevinputchar[0] == 32) {

		  // ok so this is slightly flawed.  For instance, when we activate emacs we get
		  // 55 55 59 50 48 56 48 53 59 48 99 99 99   27   93 49 49 59 114 103 98 58 48 48 48 48 47 48 48 48 48 47 48 48 48 48   27  92
		  // 7  7  ;  2  0  8  0  5  ;  0  c  c  c  ESC    ]  1  1  ;   r   g  b  :  0  0  0  0  /  0  0  0  0  /  0  0  0  0  ESC \
		  //
		  // (line 1 is the ints, line 2 is the translation)
		  // ... incoming from emacs onto our inputchar sequence.  This is some part of emacs setting up their graphical terminal and
		  // it is using the exact same sequence as our desired hotkeys.
		  // I think that the solution here is going to be to have a three key sequence - instead of ESC \ we need to do "space ESC \"

		  //printf("@@wtf why is emacs triggering this");
        printf("\033[32m|\033[39m");

		//@@ ok this is interesting.
		//system("stty -a");
		// @@ the stty -a *correctly* gets the terminal size from this position for so we could theoretically share it with the child.
		// but... the whole thing dies when the system() call exits for some reaspon
        fflush(stdout);

        // we go into special mode here
        in_magic_pipe_ready_mode = 1;
        in_magic_pipe_reading_mode = 0;

        tcsetattr (fileno(stdin), TCSANOW, &parent_orig_term_settings); 
        fcntl (0, F_SETFL, ~O_NONBLOCK);

        rc = read(fileno(stdin), pipe_argument, sizeof(pipe_argument));
        if (rc > 0) {
            pipe_argument[rc] = '\0';
        }

        inputchar[0] = 13;
        write(fdm, inputchar, sizeof(inputchar));  // send an enter to avoid double enter

        fcntl (0, F_SETFL, O_NONBLOCK);
        tcsetattr (fileno(stdin), TCSANOW, &new_term_settings); 

        continue;
      } 

      // send the previously held character, then continue to send inputchar as normal
      if (previnputchar[0] == 27 && inputchar[0] != 92) {
        write(fdm, previnputchar,sizeof(previnputchar));
      }

      if (inputchar[0] == 13 && in_magic_pipe_ready_mode == 1) {
        // This is where we switch our pipe into the temp file
        tmpnam(tmp_name);
        tmp_file = fopen(tmp_name, "w");

        in_magic_pipe_ready_mode = 0;
        in_magic_pipe_reading_mode = 1; 

        gettimeofday(&t1,0);
      }

      if (rc > 0) {
        write(fdm,inputchar,sizeof(inputchar));
      }

      // for select, have to reset these every time
      fd_set s_rd, s_wr, s_ex;
      FD_ZERO(&s_rd);
      FD_ZERO(&s_wr);
      FD_ZERO(&s_ex);
      FD_SET(fdm, &s_rd);

      if (select(fdm+1, &s_rd, &s_wr, &s_ex, &tv)) {

        if (FD_ISSET(fdm, &s_rd)) {

          rc = read(fdm, input, sizeof(input) - 1); 

          if (rc > 0) 
          { 
            // Make the answer NUL terminated to display it as a string
            input[rc] = '\0'; 

            if (in_magic_pipe_reading_mode == 1) {

              fwrite(input, 1, rc, tmp_file);
              
            } else {

              fprintf(stderr, "%s", input); 
            }

            // we just got some data, so we reset the clock
            gettimeofday(&t1,0);

          }
        }
      } 

      if (in_magic_pipe_reading_mode == 1) {
        
        gettimeofday(&t2,0);
        double secs = (double)(t2.tv_usec - t1.tv_usec) / 1000000 + (double)(t2.tv_sec - t1.tv_sec);

        if (secs > .2) {

          fclose(tmp_file);

          //printf("@@ .2 s elapsed, so this is the end of the text we'd buffer/grab");

          char cmdbuffer[4096];
          sprintf(cmdbuffer, "cat %s | %s", tmp_name, pipe_argument);

          // remove that \n
          size_t ln = strlen(cmdbuffer) - 1;
          if (*cmdbuffer && cmdbuffer[ln] == '\n') {
              cmdbuffer[ln] = '\0';
            }

          // we temporarily restore the terminal becuase it confuses system()
          tcsetattr (fileno(stdin), TCSANOW, &parent_orig_term_settings); 

          global_ignore_child_handler = 1;

		  //printf("\033[32m-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-\033[39m\n");
		  printbar(80);
          system(cmdbuffer);
		  printf("\n");
		  printbar(80);
  		  //printf("\n\033[32m-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-\033[39m\n");
		  
          global_ignore_child_handler = 0;

          tcsetattr (fileno(stdin), TCSANOW, &new_term_settings); 

          // we're done with the temporary file at this point
          unlink(tmp_name);
          
          // back to default modes
          in_magic_pipe_reading_mode = 0;
          in_magic_pipe_ready_mode = 0;

          // finally, send a new prompt.  Does this cause any trouble with it being an extra enter?@@
            inputchar[0] = 13;
            write(fdm, inputchar, sizeof(inputchar));  // send an enter to avoid double enter
        }
      }
    }
  }  

else 
{ 
  // Child

  //int childpid = getpid();
  setsid();

  // Close the master side of the PTY 
  close(fdm); 

  // The slave side of the PTY becomes the standard input and outputs of the child process 
  close(0); // Close standard input (current terminal) 
  close(1); // Close standard output (current terminal) 
  close(2); // Close standard error (current terminal) 

  dup(fds); // PTY becomes standard input (0) 
  dup(fds); // PTY becomes standard output (1) 
  dup(fds); // PTY becomes standard error (2)

  // @@ at some point I would like to have the shell be configurable at the command line,
  // but for now I have (for instance) problems with up-arrow and such not working correctly
  // in csh
  execl("/bin/bash", NULL);

} 

return 0; 
} // main
