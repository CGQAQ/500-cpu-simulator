#include <stdio.h>

#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include "stdint.h"

using namespace std;

// constants for our processor definition (sizes are in words)
#define WORD_SIZE 2
#define DATA_SIZE 1024
#define CODE_SIZE 1024
#define REGISTERS 16

///////////////////////////////////////////////
// constants and structures
static int16_t registers_general[REGISTERS];
static uint16_t register_pc;

// our opcodes are nicely incremental
enum OPCODES {
  ADD_OPCODE,
  SUB_OPCODE,
  AND_OPCODE,
  OR_OPCODE,
  XOR_OPCODE,
  MOVE_OPCODE,
  SHIFT_OPCODE,
  BRANCH_OPCODE,
  NUM_OPCODES
};

typedef enum OPCODES Opcode;

// ------------------------------debug----------------

const static char *OPCODES_STR[]{"ADD",  "SUB",   "AND",    "OR", "XOR",
                                 "MOVE", "SHIFT", "BRANCH", "NUM"};

const static char *OPCODES_BRANCH_STR[]{
    "JR", "BEQ", "BNE", "BLT", "BGT", "BLE", "BGE",
};

// ---------------------------------------------------

// We have specific phases that we use to execute each instruction.
// We use this to run through a simple state machine that always advances to the
// next state and then cycles back to the beginning.
enum PHASES {
  FETCH_INSTR,
  DECODE_INSTR,
  CALCULATE_EA,
  FETCH_OPERANDS,
  EXECUTE_INSTR,
  WRITE_BACK,
  NUM_PHASES,
  // the following are error return codes that the state machine may return
  ILLEGAL_OPCODE,   // indicates that we can't execute anymore instructions
  INFINITE_LOOP,    // indicates that we think we have an infinite loop
  ILLEGAL_ADDRESS,  // inidates that we have an memory location that's out of
                    // range
};

typedef enum PHASES Phase;

// standard function pointer to run our control unit state machine
typedef Phase (*process_phase)(void);

///////////////////////////////////////////////
// prototypes
Phase fetch_instr();

Phase decode_instr();

Phase calculate_ea();

Phase fetch_operands();

Phase execute_instr();

Phase write_back();

////////////////////////////////////////////////
// local variables

static uint8_t *g_current_inst_raw;
static uint8_t g_current_inst;
static int16_t *g_current_operand_left;
static int16_t *g_current_operand_right;
static bool g_current_operand_right_need_fetch;
static int16_t g_current_operand_right_fetched;

// memory for our code and data, using our word size for a second dimension to
// make accessing bytes easier
static uint8_t code[CODE_SIZE][WORD_SIZE];
static uint8_t data[DATA_SIZE][WORD_SIZE];

// A list of handlers to process each state. Provides for a nice simple
// state machine loop and is easily extended without using a huge
// switch statement.
static process_phase control_unit[NUM_PHASES] = {fetch_instr,   decode_instr,
                                                 calculate_ea,  fetch_operands,
                                                 execute_instr, write_back};

void print_inst(uint8_t inst, uint8_t left, uint8_t right) {
  stringstream instruction;
  int operand_left = static_cast<int>(left);
  int operand_right = static_cast<int>(right);
  auto opcode_category = inst >> 3 & 0b111;
  auto opcode_type = inst & 0b111;
  switch (opcode_category) {
    case SHIFT_OPCODE:
      // 110 is used to identify a shift operation.
      // The remaining 3 bits are used to identify direction,
      // with 000 indicating right and 001 indicating left.
      if (opcode_type == 0) {
        instruction << "SRR";
      } else if (opcode_type == 1) {
        instruction << "SRL";
      }
      break;
    case BRANCH_OPCODE:
      // Each operation will have the following format:
      // 111    xxx    _ _ _ _     _ _ _ _ _ _
      // The operation value assignments are:
      // • 000: JR (note that operand 2 will contain all zeros)
      // • 001: BEQ
      // • 010: BNE
      // • 011: BLT
      // • 100: BGT
      // • 101: BLE
      // • 110: BGE
      if (opcode_type < 0b111) {
        instruction << OPCODES_BRANCH_STR[opcode_type];
      }
      break;
    default:
      instruction << OPCODES_STR[opcode_category];
  }
  instruction << " ";

  switch (opcode_category) {
    case MOVE_OPCODE:
      if (opcode_type == 0) {
        // 000: literal to register with format:
        instruction << "R" << operand_left << "," << operand_right;
      } else if (opcode_type == 1) {
        // 001: memory to register with format:
        instruction << "R" << operand_left << ","
                    << "[R" << (operand_right >> 2 & 0b1111) << "]";
      } else if (opcode_type == 0b100) {
        // 100: literal to memory with format:
        instruction << "[R" << operand_left << "]," << operand_right;
      } else if (opcode_type == 0b101) {
        // 101: register to memory with format:
        instruction << "[R" << operand_left << "],R"
                    << (operand_right >> 2 & 0b1111);
      }
      break;
    case SHIFT_OPCODE:
      instruction << "R" << operand_left;
      break;
    case BRANCH_OPCODE:
      instruction << "R" << operand_left;
      if (opcode_type != 0) {
        instruction << "," << operand_right;
      }
      break;
    default:
      if (opcode_type == 0) {
        // xxx    000    _ _ _ _    _ _ _ _ _ _
        instruction << "R" << operand_left << "," << operand_right;
      } else if (opcode_type == 1) {
        // xxx    001    _ _ _ _    _ _ _ _ 0 0
        instruction << "R" << operand_left << ",R"
                    << (operand_right >> 2 & 0b1111);
      }
  }
  string a = instruction.str();
  cout << a << "\n";
}

/////////////////////////////////////////////////
// state processing routines
Phase fetch_instr() {
  g_current_inst_raw = code[register_pc];
  return DECODE_INSTR;
}

Phase decode_instr() {
  // big endian
  auto &l = g_current_inst_raw[0];
  auto &r = g_current_inst_raw[1];
  g_current_inst = l >> 2 & 0b111111;
  auto opcode_category = g_current_inst >> 3 & 0b111;
  auto opcode_type = g_current_inst & 0b111;

  g_current_operand_left =
      &registers_general[(l << 2 & 0b1100) | (r >> 6 & 0b11)];
  //  g_current_operand_right = r & 0b111111;
  switch (opcode_category) {
    case MOVE_OPCODE:
      if (opcode_type == 1 || opcode_type == 0b101) {
        g_current_operand_right = &registers_general[r >> 2 & 0b1111];
        g_current_operand_right_need_fetch = true;
      } else if (opcode_type == 0 || opcode_type == 0b100) {
        g_current_operand_right = nullptr;
        g_current_operand_right_need_fetch = false;
        g_current_operand_right_fetched = r & 0b111111;
      } else {
        return ILLEGAL_OPCODE;
      }
      break;
    case ADD_OPCODE:
    case SUB_OPCODE:
    case AND_OPCODE:
    case OR_OPCODE:
    case XOR_OPCODE:
      if (opcode_type == 0) {
        g_current_operand_right = nullptr;
        g_current_operand_right_need_fetch = false;
        g_current_operand_right_fetched = r & 0b111111;
      } else if (opcode_type == 1) {
        g_current_operand_right = &registers_general[r >> 2 & 0b1111];
        g_current_operand_right_need_fetch = true;
      } else {
        return ILLEGAL_OPCODE;
      }
      break;
    default:
      g_current_operand_right = nullptr;
      g_current_operand_right_need_fetch = false;
      g_current_operand_right_fetched = r & 0b111111;
  }
  print_inst(g_current_inst, (l << 2 & 0b1100) | (r >> 6 & 0b11), r & 0b111111);
  return CALCULATE_EA;
}

Phase calculate_ea() { return FETCH_OPERANDS; }

Phase fetch_operands() {
  if (g_current_operand_right_need_fetch) {
    auto d = data[*g_current_operand_right];
    g_current_operand_right_fetched = (d[1] << 8 & 0b111111110000000) | d[0];
  }
  return EXECUTE_INSTR;
}

Phase execute_instr() {
  auto opcode_category = g_current_inst >> 3 & 0b111;
  auto opcode_type = g_current_inst & 0b111;
  switch (opcode_category) {
    case ADD_OPCODE:
      *g_current_operand_left += g_current_operand_right_fetched;
      break;
    case SUB_OPCODE:
      *g_current_operand_left -= g_current_operand_right_fetched;
      break;
    case AND_OPCODE:
      *g_current_operand_left &= g_current_operand_right_fetched;
      break;
    case OR_OPCODE:
      *g_current_operand_left |= g_current_operand_right_fetched;
      break;
    case XOR_OPCODE:
      *g_current_operand_left ^= g_current_operand_right_fetched;
      break;
    case MOVE_OPCODE:
      *g_current_operand_left = g_current_operand_right_fetched;
      break;
    case SHIFT_OPCODE:
      if (opcode_type == 0)
        *g_current_operand_left >>= 1;
      else if (opcode_type == 1)
        *g_current_operand_left <<= 1;
      else
        return ILLEGAL_OPCODE;
      break;
    case BRANCH_OPCODE:
      switch (opcode_type) {
        case 0:  // JR direct jump
          register_pc = *g_current_operand_left;
          break;
        case 1:  // BEQ
          if (*g_current_operand_left == registers_general[0]) {
            register_pc += g_current_operand_right_fetched;
          }
          break;
        case 2:  // BNE
          if (*g_current_operand_left != registers_general[0]) {
            register_pc += g_current_operand_right_fetched;
          }
          break;
        case 3:  // BLT
          if (*g_current_operand_left < registers_general[0]) {
            register_pc += g_current_operand_right_fetched;
          }
        case 4:  // BGT
          if (*g_current_operand_left > registers_general[0]) {
            register_pc += g_current_operand_right_fetched;
          }
          break;
        case 5:  // BLE
          if (*g_current_operand_left <= registers_general[0]) {
            register_pc += g_current_operand_right_fetched;
          }
          break;
        case 6:  // BGE
          if (*g_current_operand_left >= registers_general[0]) {
            register_pc += g_current_operand_right_fetched;
          }
          break;
        default:
          return ILLEGAL_OPCODE;
      }
    default:
      return ILLEGAL_OPCODE;
  }
  register_pc++;
  return WRITE_BACK;
}

Phase write_back() { return FETCH_INSTR; }

/////////////////////////////////////////////////
// general routines

void initialize_system() {
  for (int i = 0; i <= REGISTERS; i++) {
    registers_general[i] = 0;
  }
  // start executing at location 0
  register_pc = 0;
  memset(data, 0xFF, sizeof data);
  memset(code, 0xFF, sizeof code);
}

void print_memory() {}

// converts the passed string into binary form and inserts it into our data
// area assumes an even number of words!!!
void insert_data(string line) {
  static int data_index = 0;
  unsigned int i;
  char ascii_data[5];
  unsigned char byte1;
  unsigned char byte2;

  ascii_data[4] = '\0';

  for (i = 0; i < line.length(); i += 4) {
    ascii_data[0] = line[i];
    ascii_data[1] = line[i + 1];
    ascii_data[2] = line[i + 2];
    ascii_data[3] = line[i + 3];
    if (data_index < DATA_SIZE * WORD_SIZE) {
      sscanf(ascii_data, "%02hhx%02hhx", &byte1, &byte2);
      data[data_index][0] = byte1;
      data[data_index++][1] = byte2;
    }
  }
}

// reads in the file data and returns true is our code and data areas are
// ready for processing
bool load_files(const char *code_filename, const char *data_filename) {
  FILE *code_file = NULL;
  std::ifstream data_file(data_filename);
  string line;  // used to read in a line of text
  bool rc = false;

  // using RAW C here since I want to have straight binary access to the data
  code_file = fopen(code_filename, "r");

  // since we're allowing anything to be specified, make sure it's a file...
  if (code_file) {
    // put the code into the code area
    fread(code, 1, CODE_SIZE * WORD_SIZE, code_file);

    fclose(code_file);

    // since we're allowing anything to be specified, make sure it's a file...
    if (data_file.is_open()) {
      // read the data into our data area
      getline(data_file, line);
      while (!data_file.eof()) {
        // put the data into the data area
        insert_data(line);

        getline(data_file, line);
      }
      data_file.close();

      // both files were read so we can continue processing
      rc = true;
    }
  }

  return rc;
}

// runs our simulation after initializing our memory
int main(int argc, const char *argv[]) {
  Phase current_phase = FETCH_INSTR;  // we always start if an instruction fetch

  initialize_system();

  // read in our code and data
  if (load_files(argv[1], argv[2])) {
    // run our simulator
    while (current_phase < NUM_PHASES)
      current_phase = control_unit[current_phase]();

    // output what stopped the simulator
    switch (current_phase) {
      case ILLEGAL_OPCODE:
        printf("Illegal instruction %02x%02x detected at address %04x\n\n",
               /*better put some data here!*/ g_current_inst_raw[0],
               g_current_inst_raw[1], register_pc);
        break;

      case INFINITE_LOOP:
        printf(
            "Possible infinite loop detected with instruction %02x%02x at "
            "address %04x\n\n",
            /*better put some data here!*/ g_current_inst_raw[0],
            g_current_inst_raw[1], register_pc);
        break;

      case ILLEGAL_ADDRESS:
        printf(
            "Illegal address %04x detected with instruction %02x%02x at "
            "address %04x\n\n",
            /*better put some data here!*/ g_current_inst_raw[0],
            g_current_inst_raw[1], register_pc);
        break;

      default:
        break;
    }

    // print out the data area
    print_memory();
  }
}
