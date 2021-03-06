/*
  Copyright notice
  ================
  
  Copyright (C) 2010 - 2013
      Lorenzo  Martignoni <martignlo@gmail.com>
      Roberto  Paleari    <roberto.paleari@gmail.com>
      Aristide Fattori    <joystick@security.di.unimi.it>
      Mattia   Pagnozzi   <pago@security.di.unimi.it>
  
  This program is free software: you can redistribute it and/or modify it under
  the terms of the GNU General Public License as published by the Free Software
  Foundation, either version 3 of the License, or (at your option) any later
  version.
  
  HyperDbg is distributed in the hope that it will be useful, but WITHOUT ANY
  WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
  A PARTICULAR PURPOSE.  See the GNU General Public License for more details.
  
  You should have received a copy of the GNU General Public License along with
  this program. If not, see <http://www.gnu.org/licenses/>.
  
*/

#include "hyperdbg_common.h"
#include "hyperdbg_host.h"
#include "hyperdbg_cmd.h"
#include "common.h"
#include "keyboard.h"
#include "video.h"
#include "x86.h"
#include "debug.h"
#include "vmmstring.h"
#include "gui.h"
#include "events.h"
#include "sw_bp.h"
#include "vt.h"
#include "symsearch.h"
#include "mmu.h"

#ifdef GUEST_WINDOWS
#include "winxp.h"
#endif

/* ################ */
/* #### MACROS #### */
/* ################ */

#define HYPERDBG_HYPERCALL_SETRES    0xdead0001
#define HYPERDBG_HYPERCALL_USER      0xdead0002

/* ########################## */
/* #### LOCAL PROTOTYPES #### */
/* ########################## */

static EVENT_PUBLISH_STATUS HyperDbgSwBpHandler(PEVENT_ARGUMENTS args);
static EVENT_PUBLISH_STATUS HyperDbgDebugHandler(PEVENT_ARGUMENTS args);
static EVENT_PUBLISH_STATUS HyperDbgIOHandler(PEVENT_ARGUMENTS args);

// static EVENT_PUBLISH_STATUS HyperDbgVMCallHandler(PEVENT_ARGUMENTS args);

#if 0
/* This hypercall is invoked when we want to change the resolution of the GUI
   dynamically but it is not used so far so I commented it out */
static EVENT_PUBLISH_STATUS HyperDbgHypercallSetResolution(PEVENT_ARGUMENTS args);
#endif

static EVENT_PUBLISH_STATUS HyperDbgHypercallUser(PEVENT_ARGUMENTS args);

static void HyperDbgEnter(void);
static void HyperDbgCommandLoop(void);

/* ##################### */
/* #### GLOBAL VARS #### */
/* ##################### */

#define DUMP_BUFFER_SIZE 100

Bit8u keyboard_history[16][256];
unsigned int history_index = 0;
hvm_bool history_full = FALSE;
int current_cmd = 0;

/* ################ */
/* #### BODIES #### */
/* ################ */

/* This hypercall will handle VMCALL #HYPERDBG_HYPERCALL_USER, it expects a
   number in RBX identifying the requested action. Depending on the action,
   further parameters can be specified in other registers or on the stack */
static EVENT_PUBLISH_STATUS HyperDbgHypercallUser(PEVENT_ARGUMENTS args)
{
  return EventPublishPass;
}

/* Not used right now */
#if 0
static EVENT_PUBLISH_STATUS HyperDbgHypercallSetResolution(PEVENT_ARGUMENTS args)
{
  VideoSetResolution(context.GuestContext.rbx, context.GuestContext.rcx);

  return EventPublishHandled;
}
#endif

static void HyperDbgEnter(void)
{
  hvm_status r;

  /* Update HyperDbg state structure */
  hyperdbg_state.enabled = TRUE;

  if(!VideoEnabled()) {
    r = VideoAlloc();
    if(r != HVM_STATUS_SUCCESS)
      Log("[HyperDbg] Cannot allocate video memory!");
  }

  Log("[HyperDbg] Entering HyperDbg command loop"); 

  HyperDbgCommandLoop();

  Log("[HyperDbg] Leaving HyperDbg command loop");

  hyperdbg_state.enabled = FALSE;
}

static void HyperDbgCommandLoop(void)
{
  hvm_address flags;
  Bit8u c, keyboard_buffer[256];
  Bit32s i;
  hvm_bool isMouse, exitLoop;
  exitLoop = FALSE;
    
  /* Backup RFLAGS */
  flags = RegGetFlags();
  /* Disable interrupts (only if they were enabled) */
  RegSetFlags(flags & ~FLAGS_IF_MASK);

#if 0
  /* Debug code for mmu read/write testing. Inject a infinite loop on guest. */
  char *buf = "\xeb\xfe";

  Log("[HyperDbgCommandLoop] Try to inject a infinite loop on guest");
  Log("[HyperDbgCommandLoop] Guest cr3 is 0x%08hx",context.GuestContext.cr3);
  Log("[HyperDbgCommandLoop] Have to replace guest resumeip with loop code, stored in buffer %08hx", buf);

  MmuWriteVirtualRegion(context.GuestContext.cr3, context.GuestContext.rip, buf, 2);
  context.GuestContext.resumerip = context.GuestContext.rip;

#endif
  
  if (VideoEnabled()) {
    /* Backup video memory */

    if (!hyperdbg_state.singlestepping)
      VideoSave();

    VideoInitShell();
  }
  
  /* We are not currently single-stepping */
  hyperdbg_state.singlestepping = FALSE;  

  /* Initialize kbd buffer */
  vmm_memset(keyboard_buffer, 0, sizeof(keyboard_buffer));
 
  i = 0;
  while (1) {
    if (KeyboardReadKeystroke(&c, FALSE, &isMouse) != HVM_STATUS_SUCCESS) {
      /* Sleep for some time, just to avoid full busy waiting */
      CmSleep(150);
      continue;
    }

    if (isMouse) {
      /* Skip mouse events */
      continue;
    }

    if (c == HYPERDBG_MAGIC_SCANCODE) {
      Log("[HyperDbg] Magic key detected! Disabling HyperDbg...");
      break;
    }
    c = KeyboardScancodeToKeycode(c);
    
    /* Process this key */
    switch (c) {
    case 0:
      /* Unrecognized key -- ignore it */
      break;

    case '\b':
      /* Backslash */
      if (i > 0) {
	keyboard_buffer[--i] = 0;
      }
      break;

    case '\n':
      /* End of command */
      
      vmm_memset(keyboard_history[history_index], 0, sizeof(keyboard_history[history_index]));
      vmm_strncpy((unsigned char *)(&keyboard_history[history_index]), (unsigned char *)(&keyboard_buffer), 256);
      
      if(history_index == 15 && !history_full) {
	history_full = TRUE;
      }

      history_index = (history_index + 1)%16;
      current_cmd = history_index;
      if(history_index == 0) current_cmd = 16;

      exitLoop = HyperDbgProcessCommand(keyboard_buffer);
      i = 0;
      vmm_memset(keyboard_buffer, 0, sizeof(keyboard_buffer));
      break;

    case 0x3:
      /* Up Arrow */
      if(history_full || current_cmd > 0) {
	if(current_cmd <= 0)
	  current_cmd = 16;
	else
	  current_cmd = (current_cmd - 1)%16;
	
	vmm_memset(keyboard_buffer, 0, sizeof(keyboard_buffer));
	VideoUpdateShell(keyboard_buffer);
	vmm_strncpy((unsigned char *)(&keyboard_buffer), (unsigned char *)(&keyboard_history[current_cmd]), 256);
	i = (int) vmm_strlen(keyboard_buffer);
	VideoUpdateShell(keyboard_buffer);
      }
      break;

    case 0x4:
      /* Down Arrow */
      if(history_full || current_cmd < history_index) {
	current_cmd = (current_cmd + 1)%16;
	
	vmm_memset(keyboard_buffer, 0, sizeof(keyboard_buffer));

	VideoUpdateShell(keyboard_buffer);
	vmm_strncpy((unsigned char *)(&keyboard_buffer), (unsigned char *)(&keyboard_history[current_cmd]), 256);
	i = (int) vmm_strlen(keyboard_buffer);
	VideoUpdateShell(keyboard_buffer);
      }
      break;

    case '\t':
      /* Replace tabs with spaces */
      c = ' ';
      /* "break" intentionally omitted! */

    default:
      /* Store this keycode. The '-1' is to ensure keyboard_buffer is always
	 null-terminated */
      if (i < (sizeof(keyboard_buffer)/sizeof(Bit8u)-1)) {
	keyboard_buffer[i++] = c;
      }
      break;
    } /* end switch */

    /* There's no need to update the GUI if buffers are the same */
    if (VideoEnabled()) {
      VideoUpdateShell(keyboard_buffer);
    }

    if(exitLoop) {
      break; /* Command says that we must return control to the guest */
    }
  }

  if(VideoEnabled() && !hyperdbg_state.singlestepping) {
    /* Restore video memory */
    VideoRestore();
  }

  /* Restore original flags */
  /* RegSetFlags(flags); */
}

/* Invoked for write operations to the keyboard I/O port */
static EVENT_PUBLISH_STATUS HyperDbgIOHandler(PEVENT_ARGUMENTS args)
{
  Bit8u c;
  hvm_bool isMouse;

  /* This handler needs an event argument! */
  if (!args || args->EventIO.size != 1 || args->EventIO.isstring || args->EventIO.isrep) {
    return EventPublishPass;
  }

  /* 'inb', from keyboard port, not rep, not string */
  if (KeyboardReadKeystroke(&c, FALSE, &isMouse) != HVM_STATUS_SUCCESS) {
    /* Something bad happened.. */
    return EventPublishPass;
  }

  /* Emulate the 'inb' instruction. The destination is always %al */
  context.GuestContext.rax = ((hvm_address) context.GuestContext.rax & 0xffffff00) | (c & 0xff);

  if (!isMouse && c == HYPERDBG_MAGIC_SCANCODE) {
    /* This is our "magic" scancode! */

    /* FIXME: quite dirty... The problem is that RIP points to the 'inb'
       instruction, that will never be executed. For this reason, the debugger
       should not highlight 'inb' as the current instruction. 
       NOTE: the "core" does not update the guest RIP with the value cached in the VMCS */
    context.GuestContext.rip += hvm_x86_ops.vt_get_exit_instr_len();

    HyperDbgEnter();
  }

  return EventPublishHandled;
}

static EVENT_PUBLISH_STATUS HyperDbgSwBpHandler(PEVENT_ARGUMENTS args)
{
  hvm_bool isCr3Dipendent, isPerm, useless;
  hvm_address ours_cr3, flags, uselesss;

#ifdef GUEST_WIN_7
  hvm_status r;
  Bit8u success = 0;
#endif

  if(hyperdbg_state.console_mode) {

    if(context.GuestContext.rip == hyperdbg_state.unlink_bp_addr) {

#ifdef GUEST_WIN_7
      r = Windows7UnlinkProc(hyperdbg_state.target_cr3, hyperdbg_state.target_pep, hyperdbg_state.target_kthread, &hyperdbg_state.dispatcher_ready_index, &success);
      if(r != HVM_STATUS_SUCCESS) Log("Error on scheduling BP");

      if(success) {
      /* Remove breakpoint for unlinking */
	r = MmuWriteVirtualRegion(context.GuestContext.cr3, hyperdbg_state.unlink_bp_addr, &hyperdbg_state.opcode_backup_unlink, sizeof(Bit8u));
      }
      /* emulate pop %edi */
      r = MmuReadVirtualRegion(context.GuestContext.cr3, context.GuestContext.rsp, &context.GuestContext.rdi, sizeof(context.GuestContext.rdi));
      if(r != HVM_STATUS_SUCCESS) Log("[HyperDbg] Special BP failed. Unable to read rsp head.");
      context.GuestContext.rsp += 4;

      context.GuestContext.resumerip = hyperdbg_state.unlink_bp_addr+1;
#endif
    }
    else if(context.GuestContext.rip == hyperdbg_state.relink_bp_addr) {

#ifdef GUEST_WIN_7          
      if(context.GuestContext.rax + context.GuestContext.rcx == hyperdbg_state.dispatcher_ready_index) {
      
	/* Relink target kthread */
	r = Windows7RelinkProc(context.GuestContext.cr3, hyperdbg_state.target_kthread, hyperdbg_state.dispatcher_ready_index);
      
	/* Remove breakpoint for relinking */
	r = MmuWriteVirtualRegion(context.GuestContext.cr3, hyperdbg_state.relink_bp_addr, &hyperdbg_state.opcode_backup_relink, sizeof(Bit8u));
      }

      /* emulate push %edi */
      context.GuestContext.rsp -= 4;
      r = MmuWriteVirtualRegion(context.GuestContext.cr3, context.GuestContext.rsp, &context.GuestContext.rdi, sizeof(context.GuestContext.rdi));

      context.GuestContext.resumerip = hyperdbg_state.relink_bp_addr+1;
#endif
    }
    else {

      Log("[HyperDbg] checking bp @%.8x", context.GuestContext.rip);
      /* We need to check also the cr3, because guest can use CopyOnWrite */
      if(SwBreakpointGetBPInfo(context.GuestContext.cr3, context.GuestContext.rip, &isCr3Dipendent, &isPerm, &ours_cr3)) {

	Log("[HyperDbg] bp is ours, %s %s", isPerm?"permanent":"", isCr3Dipendent?"and cr3dipendent":"");	
	/* Update guest RIP to re-execute the faulty instruction */
	context.GuestContext.resumerip = context.GuestContext.rip;

	if(!isPerm) {

	  if(isCr3Dipendent && context.GuestContext.cr3 != ours_cr3) {
	    /* It's not the context we want, so we simply exec this istruction and reinsert BP, like a perm BP with wrong cr3 */
	    /* Log("[HyperDbg] bp is ours but cr3 is wrong...ours is 0x%08hx", ours_cr3); */
	    goto PermBP;
	  }

	  SwBreakpointDelete(context.GuestContext.cr3, context.GuestContext.rip);

	  HyperDbgEnter();
	}
	else {
PermBP:
	  if(!isCr3Dipendent || (isCr3Dipendent && context.GuestContext.cr3 == ours_cr3)) {
	    SwBreakpointDeletePerm(context.GuestContext.cr3, context.GuestContext.rip);
	    HyperDbgEnter();
	  }

	  if(SwBreakpointGetBPInfo(context.GuestContext.cr3, context.GuestContext.rip, &useless, &useless, &uselesss)) {
	  
	    SwBreakpointDelete(ours_cr3, context.GuestContext.rip);

	    if(!hyperdbg_state.singlestepping) {
	      /* Fetch guest EFLAGS */
	      flags = context.GuestContext.rflags;
	  
	      /* Enable single-step, but don't trap current instruction */
	      flags |= FLAGS_TF_MASK;
	      flags |= FLAGS_RF_MASK;
	      /* Temporarly disable interrupts in the guest so that the single-stepping instruction isn't interrupted */
	      flags &= ~FLAGS_IF_MASK;
	      /* Store info on old flags */
	      hyperdbg_state.TF_on = (context.GuestContext.rflags & FLAGS_TF_MASK) != 0 ? TRUE : FALSE;
	      hyperdbg_state.IF_on = (context.GuestContext.rflags & FLAGS_IF_MASK) != 0 ? TRUE : FALSE;

	      context.GuestContext.rflags = flags;	    
	    }

	    /* When catch #DB, we have to check if we are in this case */
	    hyperdbg_state.hasPermBP = TRUE;
	    hyperdbg_state.previous_codeaddr = context.GuestContext.rip;
	    hyperdbg_state.isPermBPCr3Dipendent = isCr3Dipendent;
	    hyperdbg_state.bp_cr3 = ours_cr3;
	  }
	  else {

	    hyperdbg_state.hasPermBP = FALSE;
	  }
	}
      }
      else return EventPublishPass;

      Log("[HyperDbg] Done!");
    }
  }
  else { 

    /* This is a BP set by an hypercall */
  }
 
  return EventPublishHandled;
}

static EVENT_PUBLISH_STATUS HyperDbgDebugHandler(PEVENT_ARGUMENTS args)
{
  hvm_address flags;
  Bit32u bp_index;

  /* Check if we are single-stepping or not. This is needed because #DB
     exceptions are also generated by the core mechanisms that handles I/O
     instructions */
  if (!hyperdbg_state.singlestepping && !hyperdbg_state.hasPermBP)
    return EventPublishPass;

  /* Restore EFLAGS and hide it to the guest */
  flags = context.GuestContext.rflags;
  
  if(hyperdbg_state.IF_on) {
    flags |= FLAGS_IF_MASK;
  }
  if(hyperdbg_state.TF_on) {
    flags |= FLAGS_TF_MASK;
  }
  else {
    flags &= ~FLAGS_TF_MASK;
  }
  context.GuestContext.rflags = flags;

  /* Update guest RIP to re-execute the faulty instruction */
  context.GuestContext.resumerip = context.GuestContext.rip;

  if(hyperdbg_state.singlestepping) {
    HyperDbgEnter();

    /* Let's set RF to 1 so that we won't trap when really executing the instruction */
    flags = context.GuestContext.rflags;
    flags |= FLAGS_RF_MASK;
    context.GuestContext.rflags = flags;
  }

  if(hyperdbg_state.hasPermBP) {
    bp_index = SwBreakpointSet(hyperdbg_state.bp_cr3, hyperdbg_state.previous_codeaddr, TRUE, hyperdbg_state.isPermBPCr3Dipendent);
    hyperdbg_state.hasPermBP = FALSE;
    /* Enable this if you have problem with permanent BPs */
    /* Log("[HyperDbg] Set again breakpoint (cr3 0x%08x, addr 0x%08hx) at index %d", hyperdbg_state.bp_cr3, hyperdbg_state.previous_codeaddr, bp_index); */
  }

  return EventPublishHandled;
}

EVENT_PUBLISH_STATUS HyperDbgIO(void)
{
  return EventPublishHandled;
}

hvm_status HyperDbgHostInit(void)
{
  EVENT_CONDITION_EXCEPTION exception;
  EVENT_CONDITION_IO io;
  EVENT_CONDITION_HYPERCALL hypercall;
  hvm_status r;

  /* Init keyboard module */
  r = KeyboardInit();
  if (r != HVM_STATUS_SUCCESS)
    return r;

  /* Register the INT3 handler */
  exception.exceptionnum = TRAP_INT3;
  if(!EventSubscribe(EventException, &exception, sizeof(exception), HyperDbgSwBpHandler)) {
    return HVM_STATUS_UNSUCCESSFUL;
  }

  /* Register the DEBUG handler */
  exception.exceptionnum = TRAP_DEBUG;
  if(!EventSubscribe(EventException, &exception, sizeof(exception), HyperDbgDebugHandler)) {
    return HVM_STATUS_UNSUCCESSFUL;
  }

  /* JOY: I commented this as for now we are not using it and we do not want
     random userspace process to mess up with our resolution :) */
#if 0
  /* Register a hypercall to update screen resolution */
  hypercall.hypernum = HYPERDBG_HYPERCALL_SETRES;
  if(!EventSubscribe(EventHypercall, &hypercall, sizeof(hypercall), HyperDbgHypercallSetResolution)) {
    GuestLog("ERROR: Unable to register screen resolution hypercall handler");
    return HVM_STATUS_UNSUCCESSFUL;
  }
#endif

  /* Register a hypercall to handle non-root -> root requests */
  hypercall.hypernum = HYPERDBG_HYPERCALL_USER;
  if(!EventSubscribe(EventHypercall, &hypercall, sizeof(hypercall), HyperDbgHypercallUser)) {
    GuestLog("ERROR: Unable to register non-root -> root hypercall handler");
    return HVM_STATUS_UNSUCCESSFUL;
  }
  
  /* Trap keyboard-related I/O instructions */
  io.direction = EventIODirectionIn;
  io.portnum = (Bit32u) KEYB_REGISTER_OUTPUT;
  if(!EventSubscribe(EventIO, &io, sizeof(io), HyperDbgIOHandler)) {
    return HVM_STATUS_UNSUCCESSFUL;
  }

  return HVM_STATUS_SUCCESS;
}

hvm_status HyperDbgHostFini(void)
{
  EVENT_CONDITION_EXCEPTION exception;
  /* Remove DEBUG handler */
  exception.exceptionnum = TRAP_DEBUG;
  EventUnsubscribe(EventException, &exception, sizeof(exception));
  /* Remove INT3 handler */  
  exception.exceptionnum = TRAP_INT3;
  EventUnsubscribe(EventException, &exception, sizeof(exception));
  return HVM_STATUS_SUCCESS;
}
