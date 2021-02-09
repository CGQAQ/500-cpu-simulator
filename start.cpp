#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>

using namespace std;

#define LINE_LENGTH 16

// constants for our processor definition (sizes are in words)
#define WORD_SIZE 2
#define DATA_SIZE 1024
#define CODE_SIZE 1024
#define REGISTERS 16
#define INFINITE_LOOP_TRIGGER_THRESHOLD (1024000)

///////////////////////////////////////////////
// constants and structures
static uint16_t registers_general[REGISTERS];
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

Phase detecting_infinite_loop();

Phase fetch_operands();

Phase execute_instr();

Phase write_back();

////////////////////////////////////////////////
// local variables

static uint8_t *g_current_inst_raw;
static uint8_t g_current_inst;
static uint16_t *g_current_operand_left;
static uint16_t *g_current_operand_right;
static bool g_current_operand_right_need_fetch;
static int16_t g_current_operand_right_fetched;

static map<uint16_t, int32_t> g_infinite_loop_detect_map;

// memory for our code and data, using our word size for a second dimension to
// make accessing bytes easier
static uint8_t code[CODE_SIZE][WORD_SIZE];
static uint8_t data[DATA_SIZE][WORD_SIZE];

// A list of handlers to process each state. Provides for a nice simple
// state machine loop and is easily extended without using a huge
// switch statement.
static process_phase control_unit[NUM_PHASES] = {
    fetch_instr,    decode_instr,  detecting_infinite_loop,
    fetch_operands, execute_instr, write_back};

static int64_t g_instruction_counter = 0;
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
  cout << "#" << g_instruction_counter << "\tPC: " << register_pc
       << "\tINST: " << a << "\n";
  g_instruction_counter++;
}

/////////////////////////////////////////////////
// state processing routines
/**
 * fetching instruction from code section (code array)
 * @return Phase enum
 */
Phase fetch_instr() {
  g_current_inst_raw = code[register_pc];
  return DECODE_INSTR;
}

/**
 * sign extending for arbitrary digits of integer
 * @param x number to be extended
 * @param bits digits
 * @return extended number
 */
int16_t sign_extend(uint16_t x, int bits) {
  uint16_t m = 1u << (bits - 1);
  return (x ^ m) - m;
}

/**
 * decode instruction so that later phase can use
 * @return Phase enum
 */
Phase decode_instr() {
  // big endian
  auto &l = g_current_inst_raw[0];
  auto &r = g_current_inst_raw[1];
  g_current_inst = l >> 2 & 0b111111;
  auto opcode_category = g_current_inst >> 3 & 0b111;
  auto opcode_type = g_current_inst & 0b111;

  g_current_operand_left =
      &registers_general[l << 2 & 0b1100 | (r >> 6 & 0b11)];
  //  g_current_operand_right = r & 0b111111;
  switch (opcode_category) {
    case MOVE_OPCODE:
      if (opcode_type == 1) {
        g_current_operand_right = &registers_general[r >> 2 & 0b1111];
        g_current_operand_right_need_fetch = true;
      } else if (opcode_type == 0b101) {
        g_current_operand_right = &registers_general[r >> 2 & 0b1111];
        g_current_operand_right_need_fetch = false;
        g_current_operand_right_fetched = *g_current_operand_right;
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
        g_current_operand_right_need_fetch = false;
        g_current_operand_right_fetched = *g_current_operand_right;
      } else {
        return ILLEGAL_OPCODE;
      }
      break;
    case BRANCH_OPCODE: {
      g_current_operand_right = nullptr;
      g_current_operand_right_need_fetch = false;
      // g_current_operand_right_fetched = sign_extend(r & 0b111111, 6);
      break;
    }
    default:
      g_current_operand_right = nullptr;
      g_current_operand_right_need_fetch = false;
      g_current_operand_right_fetched = r & 0b111111;
  }
  // debug only, print the instruction that will be execute
  // print_inst(g_current_inst, (l << 2 & 0b1100) | (r >> 6 & 0b11), r &
  // 0b111111);
  return CALCULATE_EA;
}

/**
 * detecting infinite loop
 * @return Phase enum
 */
Phase detecting_infinite_loop() {
  // detecting infinite loop
  if (g_infinite_loop_detect_map.find(register_pc) ==
      g_infinite_loop_detect_map.end()) {
    g_infinite_loop_detect_map[register_pc] = 0;
  }
  g_infinite_loop_detect_map[register_pc] += 1;
  if (g_infinite_loop_detect_map[register_pc] >
      INFINITE_LOOP_TRIGGER_THRESHOLD) {
    return INFINITE_LOOP;
  }
  return FETCH_OPERANDS;
}

/**
 * fetch from memory (data array)
 * @return Phase enum
 */
Phase fetch_operands() {
  if (g_current_operand_right_need_fetch) {
    if (*g_current_operand_right >= DATA_SIZE || *g_current_operand_right < 0) {
      return ILLEGAL_ADDRESS;
    }
    auto d = data[*g_current_operand_right];
    g_current_operand_right_fetched = (d[0] << 8 & 0b111111110000000) | d[1];
  }
  g_current_operand_right_fetched =
      sign_extend(g_current_operand_right_fetched, 6);
  return EXECUTE_INSTR;
}

/**
 * executing decoded instruction
 * @return Phase enum
 */
Phase execute_instr() {
  auto opcode_category = g_current_inst >> 3 & 0b111;
  auto opcode_type = g_current_inst & 0b111;
  bool is_jumped = false;
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
      if (opcode_type == 0b100 || opcode_type == 0b101) {
        auto offset = *g_current_operand_left;
        if (offset >= 0 && offset < DATA_SIZE) {
          // big endian
          data[offset][0] = g_current_operand_right_fetched >> 8 & 0xFF;
          data[offset][1] = g_current_operand_right_fetched & 0xFF;
        } else {
          return ILLEGAL_ADDRESS;
        }
      } else if (opcode_type == 0 || opcode_type == 1) {
        *g_current_operand_left = g_current_operand_right_fetched;
      } else {
        return ILLEGAL_OPCODE;
      }
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
          register_pc = *g_current_operand_left - 1;
          is_jumped = true;
          break;
        case 1:  // BEQ
          if (*g_current_operand_left == registers_general[0]) {
            register_pc += g_current_operand_right_fetched;
            is_jumped = true;
          }
          break;
        case 2:  // BNE
          if (*g_current_operand_left != registers_general[0]) {
            register_pc += g_current_operand_right_fetched;
            is_jumped = true;
          }
          break;
        case 3:  // BLT
          if (*g_current_operand_left < registers_general[0]) {
            register_pc += g_current_operand_right_fetched;
            is_jumped = true;
          }
          break;
        case 4:  // BGT
          if (*g_current_operand_left > registers_general[0]) {
            register_pc += g_current_operand_right_fetched;
            is_jumped = true;
          }
          break;
        case 5:  // BLE
          if (*g_current_operand_left <= registers_general[0]) {
            register_pc += g_current_operand_right_fetched;
            is_jumped = true;
          }
          break;
        case 6:  // BGE
          if (*g_current_operand_left >= registers_general[0]) {
            register_pc += g_current_operand_right_fetched;
            is_jumped = true;
          }
          break;
        default:
          return ILLEGAL_OPCODE;
      }
      break;
    default:
      return ILLEGAL_OPCODE;
  }
  if (opcode_category != BRANCH_OPCODE || !is_jumped) register_pc++;
  return WRITE_BACK;
}

/**
 * not used here
 * @return Phase enum
 */
Phase write_back() { return FETCH_INSTR; }

/////////////////////////////////////////////////
// general routines

/**
 * initialise the code and the data array before loading data from file.
 */
void initialize_system() {
  for (int i = 0; i <= REGISTERS; i++) {
    registers_general[i] = 0;
  }
  // start executing at location 0
  register_pc = 0;
  memset(data, 0xFF, sizeof data);
  memset(code, 0xFF, sizeof code);
}

// checks the hex value to ensure it a printable ASCII character. If
// it isn't, '.' is returned instead of itself
char valid_ascii(unsigned char hex_value) {
  if (hex_value < 0x21 || hex_value > 0x7e) hex_value = '.';

  return (char)hex_value;
}

// takes the data and prints it out in hexadecimal and ASCII form
void print_formatted_data(unsigned char *data, int length) {
  int i, j, k;
  char the_text[LINE_LENGTH + 1];

  // print each line 1 at a time
  for (i = 0; i < length; i += LINE_LENGTH) {
    printf("%08x  ", i);
    // add 1 word at a time, but don't go beyond the end of the data
    for (j = 0; j < LINE_LENGTH && (i + j) < length; j += 2) {
      the_text[j] = valid_ascii(data[i + j]);
      the_text[j + 1] = valid_ascii(data[i + j + 1]);
      printf("%02x %02x ", data[i + j], data[i + j + 1]);
    }

    // add in FFFF (invalid operation) to fill out the line
    if ((i + j) >= length) {
      for (k = j; k < LINE_LENGTH; k += 2) {
        the_text[k] = valid_ascii(0xff);
        the_text[k + 1] = valid_ascii(0xff);
        printf("ff ff ");
      }
    }

    the_text[LINE_LENGTH] = '\0';
    printf(" |%s|\n", the_text);
  }
}

void print_memory() {
  print_formatted_data(reinterpret_cast<unsigned char *>(data), sizeof data);
}

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
            /*better put some data here!*/ register_pc, g_current_inst_raw[0],
            g_current_inst_raw[1], register_pc);
        break;

      default:
        break;
    }

    // print out the data area
    print_memory();
  }
}
