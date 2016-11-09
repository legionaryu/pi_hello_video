/*
Copyright (c) 2012, Broadcom Europe Ltd
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:
    * Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in the
      documentation and/or other materials provided with the distribution.
    * Neither the name of the copyright holder nor the
      names of its contributors may be used to endorse or promote products
      derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY
DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

// Video deocode demo using OpenMAX IL though the ilcient helper library

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <unistd.h>
#include <sys/select.h>

#include "bcm_host.h"
#include "ilclient.h"


   char next_filename[256];

int inputAvailable()
{
  struct timeval tv;
  fd_set fds;
  tv.tv_sec = 0;
  tv.tv_usec = 0;
  FD_ZERO(&fds);
  FD_SET(STDIN_FILENO, &fds);
  select(STDIN_FILENO+1, &fds, NULL, NULL, &tv);
  return (FD_ISSET(0, &fds));
}

static int video_decode_test(char *filename, int loop)
{
   OMX_VIDEO_PARAM_PORTFORMATTYPE format;
   OMX_TIME_CONFIG_CLOCKSTATETYPE cstate;
   COMPONENT_T *video_decode = NULL, *video_scheduler = NULL, *video_render = NULL, *clock = NULL;
   COMPONENT_T *list[5];
   TUNNEL_T tunnel[4];
   ILCLIENT_T *client;
   FILE *in;
   int status = 0;
   unsigned int data_len = 0;
   unsigned int readsize = 0;
   unsigned int filesize = 0;
   unsigned int stdin_len = 0;
   char stdin_buffer[256];
   int done_once = 0;

   memset(stdin_buffer, 0, sizeof(stdin_buffer));
   memset(list, 0, sizeof(list));
   memset(tunnel, 0, sizeof(tunnel));

   if((in = fopen(filename, "rb")) == NULL)
   {
      printf("fopen filename: %s\nerror: %s\n", filename, strerror(errno));
      return -2;
   }
   memset(next_filename, 0, sizeof(next_filename));

   if((client = ilclient_init()) == NULL)
   {
      fclose(in);
      return -3;
   }

   if(OMX_Init() != OMX_ErrorNone)
   {
      ilclient_destroy(client);
      fclose(in);
      return -4;
   }

   // create video_decode
   if(ilclient_create_component(client, &video_decode, "video_decode", ILCLIENT_DISABLE_ALL_PORTS | ILCLIENT_ENABLE_INPUT_BUFFERS) != 0)
      status = -14;
   list[0] = video_decode;

   // create video_render
   if(status == 0 && ilclient_create_component(client, &video_render, "video_render", ILCLIENT_DISABLE_ALL_PORTS) != 0)
      status = -14;
   list[1] = video_render;

   // create clock
   if(status == 0 && ilclient_create_component(client, &clock, "clock", ILCLIENT_DISABLE_ALL_PORTS) != 0)
      status = -14;
   list[2] = clock;

   memset(&cstate, 0, sizeof(cstate));
   cstate.nSize = sizeof(cstate);
   cstate.nVersion.nVersion = OMX_VERSION;
   cstate.eState = OMX_TIME_ClockStateWaitingForStartTime;
   cstate.nWaitMask = 1;
   if(clock != NULL && OMX_SetParameter(ILC_GET_HANDLE(clock), OMX_IndexConfigTimeClockState, &cstate) != OMX_ErrorNone)
      status = -13;

   // create video_scheduler
   if(status == 0 && ilclient_create_component(client, &video_scheduler, "video_scheduler", ILCLIENT_DISABLE_ALL_PORTS) != 0)
      status = -14;
   list[3] = video_scheduler;

   set_tunnel(tunnel, video_decode, 131, video_scheduler, 10);
   set_tunnel(tunnel+1, video_scheduler, 11, video_render, 90);
   set_tunnel(tunnel+2, clock, 80, video_scheduler, 12);

   // setup clock tunnel first
   if(status == 0 && ilclient_setup_tunnel(tunnel+2, 0, 0) != 0)
      status = -15;
   else
      ilclient_change_component_state(clock, OMX_StateExecuting);

   if(status == 0)
      ilclient_change_component_state(video_decode, OMX_StateIdle);

   memset(&format, 0, sizeof(OMX_VIDEO_PARAM_PORTFORMATTYPE));
   format.nSize = sizeof(OMX_VIDEO_PARAM_PORTFORMATTYPE);
   format.nVersion.nVersion = OMX_VERSION;
   format.nPortIndex = 130;
   format.eCompressionFormat = OMX_VIDEO_CodingAVC;

   if(status == 0 &&
      OMX_SetParameter(ILC_GET_HANDLE(video_decode), OMX_IndexParamVideoPortFormat, &format) == OMX_ErrorNone &&
      ilclient_enable_port_buffers(video_decode, 130, NULL, NULL, NULL) == 0)
   {
      OMX_BUFFERHEADERTYPE *buf;
      int port_settings_changed = 0;
      int first_packet = 1;

      ilclient_change_component_state(video_decode, OMX_StateExecuting);

      fseek(in, 0L, SEEK_END);
      filesize = ftell(in);
      fseek(in, 0L, SEEK_SET);

      for(;;)
      {
         // printf("=========================> 1\n");
         if(inputAvailable())
         {
            char c = 0;
            int i = stdin_len = 0;
            for (i = 0; (c = getchar()) != '\n' && c != EOF && i < 255; ++i)
            {
               stdin_buffer[i] = c;
               stdin_len = i+1;
            }
            stdin_buffer[stdin_len] = 0;
            printf("input available: %s\r\n", stdin_buffer);

            if(stdin_buffer[0] == 'q' || stdin_buffer[0] == 'Q')
            {
               printf("QUIT!\r\n");
               break;
            }
            else if(stdin_buffer[0] == 'f' || stdin_buffer[0] == 'F')
            {
               for(i = stdin_len-1; i>=0 && (stdin_buffer[i] == '\r' || stdin_buffer[i] == '\n'); i--)
               {
                  stdin_buffer[i] = 0;
               }
               int begin = 1;
               for (i = begin; i < stdin_len && stdin_buffer[i] == ' '; ++i)
               {
                  begin = i+1;
               }
               memset(next_filename, 0, sizeof(next_filename));
               strcpy(next_filename, &stdin_buffer[begin]);
               if(strlen(next_filename) > 0)
               {
                  status = 42; //play next video
                  //break;
                  //++++++++++++++++++++++++++++++++++++++++++++++++
                  // int errorCode;
                  // OMX_SendCommand(ILC_GET_HANDLE(video_decode),OMX_CommandFlush,130,NULL);
                  // OMX_SendCommand(ILC_GET_HANDLE(video_decode),OMX_CommandFlush,131,NULL);
                  // ilclient_wait_for_event(video_decode, OMX_EventCmdComplete, OMX_CommandFlush, 0, 130, 0, ILCLIENT_PORT_FLUSH, -1);
                  // ilclient_wait_for_event(video_decode, OMX_EventCmdComplete, OMX_CommandFlush, 0, 131, 0, ILCLIENT_PORT_FLUSH, -1);

                  // data_len=0;

                  // memset(&cstate, 0, sizeof(cstate));
                  // cstate.nSize = sizeof(cstate);
                  // cstate.nVersion.nVersion = OMX_VERSION;
                  // cstate.eState = OMX_TIME_ClockStateStopped;
                  // cstate.nWaitMask = 1;
                  // errorCode = OMX_SetParameter(ILC_GET_HANDLE(clock), OMX_IndexConfigTimeClockState, &cstate);

                  // if(errorCode)
                  // {
                  //    printf("OMX_TIME_ClockStateStopped errorCode: %d\n", errorCode);
                  //    // fprintf(stderr, "OMX_TIME_ClockStateStopped errorCode: %d\n", errorCode);
                  //    return errorCode;
                  // }

                  // memset(&cstate, 0, sizeof(cstate));
                  // cstate.nSize = sizeof(cstate);
                  // cstate.nVersion.nVersion = OMX_VERSION;
                  // cstate.eState = OMX_TIME_ClockStateWaitingForStartTime;
                  // cstate.nWaitMask = 1;
                  // errorCode = OMX_SetParameter(ILC_GET_HANDLE(clock), OMX_IndexConfigTimeClockState, &cstate);

                  // if(errorCode)
                  // {
                  //    printf("OMX_TIME_ClockStateWaitingForStartTime errorCode: %d\n", errorCode);
                  //    // fprintf(stderr, "OMX_TIME_ClockStateWaitingForStartTime errorCode: %d\n", errorCode);
                  //    return errorCode;
                  // }

                  // ilclient_change_component_state(clock, OMX_StateExecuting);
                  // first_packet = 1;

                  // fclose(in);
                  // if((in = fopen(next_filename, "rb")) == NULL)
                  // {
                  //    printf("fopen filename: %s\nerror: %s\n", next_filename, strerror(errno));
                  //    //perror("fopen next_filename");
                  //    return -2;
                  // }
                  // memset(next_filename, 0, sizeof(next_filename));
                  // done_once = 0;
                  // readsize = 0;

                  // ilclient_change_component_state(video_decode, OMX_StateExecuting);

                  // fseek(in, 0L, SEEK_END);
                  // filesize = ftell(in);
                  // fseek(in, 0L, SEEK_SET);
               }
            }
         }
         if(!done_once && (buf = ilclient_get_input_buffer(video_decode, 130, 1)) != NULL)
         {
            // feed data and wait until we get port settings changed
            unsigned char *dest = buf->pBuffer;
            data_len += fread(dest, 1, buf->nAllocLen-data_len, in);

            if(port_settings_changed == 0 &&
               ((data_len > 0 && ilclient_remove_event(video_decode, OMX_EventPortSettingsChanged, 131, 0, 0, 1) == 0) ||
                (data_len == 0 && ilclient_wait_for_event(video_decode, OMX_EventPortSettingsChanged, 131, 0, 0, 1,
                                                          ILCLIENT_EVENT_ERROR | ILCLIENT_PARAMETER_CHANGED, 10000) == 0)))
            {
               port_settings_changed = 1;
               if(ilclient_setup_tunnel(tunnel, 0, 0) != 0)
               {
                  status = -7;
                  break;
               }

               ilclient_change_component_state(video_scheduler, OMX_StateExecuting);
               // now setup tunnel to video_render
               if(ilclient_setup_tunnel(tunnel+1, 0, 1000) != 0)
               {
                  status = -12;
                  break;
               }

               ilclient_change_component_state(video_render, OMX_StateExecuting);
            }
            if(!data_len) {
               // Finished reading the file, either loop or exit.
               if (loop) {
                  fseek(in, 0, SEEK_SET);
               }
               else {
                  if(!done_once)
                  {
                     printf("done!\r\n");
                     done_once = 1;
                  }
                  if(strlen(next_filename) > 0)
                  {
                     fclose(in);
                     if((in = fopen(next_filename, "rb")) == NULL)
                     {
                        printf("fopen filename: %s\nerror: %s\n", next_filename, strerror(errno));
                        //perror("fopen next_filename");
                        status = -2;
                        break;
                     }
                     memset(next_filename, 0, sizeof(next_filename));
                     done_once = 0;
                     readsize = 0;
                     data_len = 0;

                     fseek(in, 0L, SEEK_END);
                     filesize = ftell(in);
                     fseek(in, 0L, SEEK_SET);
                  }
                  else
                  {
                     status = 0;
                     break;
                  }
                  // usleep(1000);
                  // continue;
               }
            }

            buf->nFilledLen = data_len;
            readsize += data_len;
            data_len = 0;

            // printf("%0.04f\r\n", (filesize == 0 ? 0 : (readsize / (float) filesize) * 100));

            buf->nOffset = 0;
            if(first_packet)
            {
               buf->nFlags = OMX_BUFFERFLAG_STARTTIME;
               first_packet = 0;
            }
            else
               buf->nFlags = OMX_BUFFERFLAG_TIME_UNKNOWN;

            if(OMX_EmptyThisBuffer(ILC_GET_HANDLE(video_decode), buf) != OMX_ErrorNone)
            {
               status = -6;
               break;
            }
         }
         // else
         // {
         //    printf("ilclient_get_input_buffer false!\r\n");
         // }
      }

      buf->nFilledLen = 0;
      buf->nFlags = OMX_BUFFERFLAG_TIME_UNKNOWN | OMX_BUFFERFLAG_EOS;

      if(OMX_EmptyThisBuffer(ILC_GET_HANDLE(video_decode), buf) != OMX_ErrorNone)
         status = -20;

      // wait for EOS from render
      ilclient_wait_for_event(video_render, OMX_EventBufferFlag, 90, 0, OMX_BUFFERFLAG_EOS, 0,
                              ILCLIENT_BUFFER_FLAG_EOS, 10000);

      // need to flush the renderer to allow video_decode to disable its input port
      ilclient_flush_tunnels(tunnel, 0);

      ilclient_disable_port_buffers(video_decode, 130, NULL, NULL, NULL);
   }

   fclose(in);

   ilclient_disable_tunnel(tunnel);
   ilclient_disable_tunnel(tunnel+1);
   ilclient_disable_tunnel(tunnel+2);
   ilclient_teardown_tunnels(tunnel);

   ilclient_state_transition(list, OMX_StateIdle);
   ilclient_state_transition(list, OMX_StateLoaded);

   ilclient_cleanup_components(list);

   OMX_Deinit();

   ilclient_destroy(client);
   return status;
}

void error_usage(char* name) {
   printf("Usage: %s [--loop] <filename>\n", name);
   exit(1);
}

int main (int argc, char **argv)
{
   int loop = 0;

   if (argc < 2 || argc > 3) {
      error_usage(argv[0]);
   }
   // Check optional parameter.
   if (argc == 3) {
      // Check for loop parameter.
      if (strcmp(argv[1], "--loop") == 0) {
         loop = 1;
      }
      // Error unknown parameter.
      else {
         error_usage(argv[0]);
      }
   }
   bcm_host_init();
   int result = 0;
   memset(next_filename, 0, sizeof(next_filename));
   strcpy(next_filename, argv[argc-1]);
   return video_decode_test(next_filename, loop);
   //printf("argv[%d]: %s\nnext_filename: %s\n", argc-1, argv[argc-1], next_filename);
   // while((result = video_decode_test(next_filename, loop)) == 42)
   // {
   //    printf("video_decode_test result: %d\n", result);
   // }
   // return result;
}


