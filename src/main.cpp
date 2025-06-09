#include "helper.h"
#include "ubc.h"
#include <cinttypes>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <sdk/os/debug.h>
#include <sdk/os/file.h>
#include <sdk/os/input.h>
#include <sdk/os/lcd.h>
#include <type_traits>

static std::size_t ticks = 0;
//static int file;
/*
extern std::uint8_t _executable_start[8];
static auto _executable_start_ptr = &_executable_start[0];
*/
static const auto launcher_ptr = reinterpret_cast<std::uint8_t *>(0x8cfe0000);

auto fix_open = reinterpret_cast<decltype(File_Open)>(0x80057f1c);
auto fix_write = reinterpret_cast<decltype(File_Write)>(0x80057f74);
auto fix_close = reinterpret_cast<decltype(File_Close)>(0x80057fa2);

__attribute__((noinline)) static void bankblockswitch() {
  std::uint32_t sr;
  __asm__ ("stc sr, %0" : "=r"(sr));
  sr ^= 1u << 29 | 1u << 28;
  __asm__ volatile ("ldc %0, sr" : : "r"(sr));
}

static void disable() {
  //AS_STRUCT_MODIFY(UBC_CBR0, .ce = false);
  //AS_STRUCT_MODIFY(UBC_CBR1, .ce = false);
  int file;
  if ((file = fix_open("\\fls0\\trace.txt", FILE_OPEN_APPEND | FILE_OPEN_WRITE)) >= 0){
    auto __attribute__((unused)) ignore = fix_write(file, "---\n", 4);
    fix_close(file);
  }
  Debug_SetCursorPosition(0, 0);
  Debug_PrintString("Disabled!", true);
  LCD_Refresh();
  Debug_WaitKey();
}

static bool do_log(int &ret, Input_Event *arg1, std::uint32_t arg2, std::uint32_t arg3, void *caller) {
  char buffer[0x1000];

  //AS_STRUCT_MODIFY(UBC_CBR0, .ce = false);
  ret = GetInput(arg1, arg2, arg3);
  //AS_STRUCT_MODIFY(UBC_CBR0, .ce = true);
  if (arg1->type == Input_EventType::EVENT_TIMER)
    ticks++;

  std::size_t length = 0;
  length += std::snprintf(buffer, sizeof(buffer), "T%3zu %p: arg1 - [ ", ticks, caller);
  for (std::size_t i = 0; i < sizeof(*arg1); i++) {
    if (length > sizeof(buffer))
      break;
    auto l =
        std::snprintf(buffer + length, sizeof(buffer) - length, "%02" PRIx8 " ",
                      reinterpret_cast<std::uint8_t *>(arg1)[i]);
    length += l;
  }
  if (length < sizeof(buffer)) {
    auto l = std::snprintf(
        buffer + length, sizeof(buffer) - length,
        "] arg2 - %3" PRIi32 " arg3 - %3" PRIi32 " = %3i\n",
        static_cast<std::make_signed_t<decltype(arg2)>>(arg2),
        static_cast<std::make_signed_t<decltype(arg3)>>(arg3), ret);
    length += l;
  }
  length = length < sizeof(buffer) ? length : sizeof(buffer);
  auto file = fix_open("\\fls0\\trace.txt", FILE_OPEN_APPEND | FILE_OPEN_WRITE);
  int fret;
  if (file >= 0) {
    fret = fix_write(file, buffer, length);
    fix_close(file);
  }
  else
   fret = 0;
  if (file < 0 || fret < 0 || static_cast<decltype(length)>(fret) != length)
    return true;

  Debug_Printf(0, 0, true, 0, "%lu", static_cast<long unsigned int>(ticks));
  LCD_Refresh();
  return false;
}

#ifndef __clang__
__attribute__((interrupt_handler, nosave_low_regs))
#endif
static void
handler() {
  void *callee;
  void *caller;
  Input_Event *arg1;
  std::uint32_t arg2;
  std::uint32_t arg3;
  int ret;
  __asm__("stc spc, %0" : "=r"(callee));
  __asm__("stc r4_bank, %0" : "=r"(arg1));
  __asm__("stc r5_bank, %0" : "=r"(arg2));
  __asm__("stc r6_bank, %0" : "=r"(arg3));
  __asm__ volatile ("sts pr, %0" : "=r"(caller));
  bankblockswitch();
  AS_STRUCT_MODIFY(UBC_CBR0, .ce = false);
  AS_STRUCT_MODIFY(UBC_CBR1, .ce = false);
  Debug_Printf(0, 1, false, 0, "%p %p", callee, caller);
  LCD_Refresh();
  //Debug_WaitKey();
  if (*launcher_ptr != 0xFF || do_log(ret, arg1, arg2, arg3, caller)) {
    disable();
    bankblockswitch();
    return;
  }
  AS_STRUCT_MODIFY(UBC_CBR0, .ce = true);
  AS_STRUCT_MODIFY(UBC_CBR1, .ce = true);
  bankblockswitch();
  __asm__ volatile("ldc %0, r0_bank" : : "r"(ret));
  __asm__ volatile("ldc %0, spc" : : "r"(caller));
}

int main() {
  AS_STRUCT_SET(
      UBC_CBR0,
      {.id = ubc_cbr0_t::UBC_CBR0_ID_INSTRUCTION_FETCH_CYCLE, .ce = false});
  AS_STRUCT_SET(UBC_CBR1, {.id = ubc_cbr1_t::UBC_CBR1_ID_OPERAND_ACCESS_CYCLE,
                           .rw = ubc_cbr1_t::UBC_CBR1_RW_BUS_WRITE_CYCLE,
                           .ce = false});
  UBC_SetUserHandler(handler);
  AS_STRUCT_MODIFY(UBC_CBCR, .ubde = true);

  *UBC_CAR0 = reinterpret_cast<void *>(GetInput);
  *UBC_CAMR0 = 0;
  *UBC_CAR1 = launcher_ptr + 4;
  *UBC_CAMR0 = 0;

  AS_STRUCT_SET(UBC_CRR0, {.bie = true});
  AS_STRUCT_SET(UBC_CRR1, {.bie = true});

  Debug_SetCursorPosition(0, 0);

  auto file = fix_open("\\fls0\\trace.txt",
                   FILE_OPEN_CREATE | FILE_OPEN_APPEND | FILE_OPEN_WRITE);
  if (file < 0 && fix_write(file, "---\n", 4) != 4) {
    Debug_PrintString("File Error!", true);
    LCD_Refresh();
    Debug_WaitKey();
    fix_close(file);
    return EXIT_FAILURE;
  }
  fix_close(file);

  *launcher_ptr = 0xFF;

  Debug_PrintString("Enabled!", true);
  LCD_Refresh();
  Debug_WaitKey();

  AS_STRUCT_MODIFY(UBC_CBR0, .ce = true);
  AS_STRUCT_MODIFY(UBC_CBR1, .ce = true);

  return EXIT_SUCCESS;
}