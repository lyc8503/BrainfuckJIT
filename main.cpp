#include <cstdio>
#include <sys/mman.h>
#include <cstring>
#include <string>
#include <iostream>
#include <vector>
#include <cassert>
#include <stack>
#include <unistd.h>
#include <fstream>
#include <sstream>

#define CELL_SIZE 4096

// allocate memory for cells
unsigned char cells[CELL_SIZE];

using namespace std;

// execute generated asm
void execute_asm(unsigned char *code, size_t len) {
    typedef unsigned (*asmFunc)();
    void *virtualCodeAddress;

    // allocate rwx memory
    virtualCodeAddress = mmap(nullptr,
            len,
            PROT_READ | PROT_WRITE | PROT_EXEC,
            MAP_ANONYMOUS | MAP_PRIVATE,
            0,
            0);

#ifndef NDEBUG
    fprintf(stderr, "virtualCodeAddress = %p\n", virtualCodeAddress);
#endif

    // copy the asm code
    memcpy(virtualCodeAddress, code, len);
    auto myFunc = (asmFunc) (virtualCodeAddress);

    // move the real rip to the target memory area and let it execute!
    myFunc();
}

// type of custom command vector
enum COMMAND_TYPE {
    DP_INC = '>',
    DP_DEC = '<',
    DATA_INC = '+',
    DATA_DEC = '-',
    INPUT = ',',
    OUTPUT = '.',
    LOOP_START = '[',
    LOOP_END = ']'
};

// one single command
struct COMMAND {
    COMMAND_TYPE type;
    int value;  // only used for inc and dec commands
};

// translate the string code to custom command sequence, and do some optimization
vector<COMMAND*> translate_command(const string& code) {
    size_t len = code.size();

    vector<COMMAND*> tmp;

    for (size_t i = 0; i < len; i++) {
        char c = code.at(i);

        if ((c == COMMAND_TYPE::DATA_INC ||
            c == COMMAND_TYPE::DATA_DEC ||
            c == COMMAND_TYPE::DP_INC ||
            c == COMMAND_TYPE::DP_DEC) &&
            !tmp.empty() &&
            tmp[tmp.size() - 1]->value < 0x7f &&
            tmp[tmp.size() - 1]->type == c) {

            // don't need a new command here
            tmp.at(tmp.size() - 1)->value++;
        } else {
            auto *cmd = new COMMAND();
            cmd->type = (COMMAND_TYPE) c;
            cmd->value = 1;
            tmp.push_back(cmd);
        }
    }
    return tmp;
}

// translate the custom command sequence to asm code
// target platform: x86_64 with linux syscall
vector<unsigned char> translate_asm(const vector<COMMAND*>& commands, const unsigned char* data) {
    vector<unsigned char> code;
    stack<unsigned long> bracketStack;

    // initialize the rdx as the data pointer
    // machine code generated by nasm `mov rdx, [the value of the pointer to data]`
    code.push_back(0x48);
    code.push_back(0xba);

    // little-endian byte order
    for (int i = 0; i < 8; i++) {
        code.push_back(((unsigned long long) data & ((unsigned long long) 0xff << (8 * i))) >> (8 * i));
    }

    for (auto cmd: commands) {
        // make sure the value fits into signed 8 bits (otherwise some machine code have different forms)
        assert(cmd->value > 0 && cmd->value <= 0x7f);

#ifndef NDEBUG
        cerr<<"Generating asm for: " << (char)cmd->type << ", " << cmd->value<<endl;
#endif
        switch (cmd->type) {
            case COMMAND_TYPE::DATA_INC:
                // machine code generated by nasm `add byte [rdx], 1`
                code.push_back(0x80);
                code.push_back(0x02);
                code.push_back((unsigned char) cmd->value);
                break;
            case COMMAND_TYPE::DATA_DEC:
                // machine code generated by nasm `sub byte [rdx], 1`
                code.push_back(0x80);
                code.push_back(0x2a);
                code.push_back((unsigned char) cmd->value);
                break;
            case COMMAND_TYPE::DP_INC:
                // machine code generated by nasm `add rdx, 1`
                code.push_back(0x48);
                code.push_back(0x83);
                code.push_back(0xc2);
                code.push_back((unsigned char) cmd->value);
                break;
            case COMMAND_TYPE::DP_DEC:
                // machine code generated by nasm `sub rdx, 1`
                code.push_back(0x48);
                code.push_back(0x83);
                code.push_back(0xea);
                code.push_back((unsigned char) cmd->value);
                break;
            case COMMAND_TYPE::LOOP_START:
                // push the current pos of the left bracket
                bracketStack.push(code.size());
                // machine code generated by nasm
                // cmp byte [rdx], 0
                // je 0x1
                code.push_back(0x80);
                code.push_back(0x3a);
                code.push_back(0x00);
                code.push_back(0x0f);
                code.push_back(0x84);
                // some "magic number" here, will be replaced when the matching right bracket was processed.
                code.push_back(0x12);
                code.push_back(0x34);
                code.push_back(0x56);
                code.push_back(0x78);
                break;
            case COMMAND_TYPE::LOOP_END: {
                // pop from the stack to find the matching left bracket
                unsigned long pos = code.size();
                unsigned long last = bracketStack.top();
                bracketStack.pop();

                long jmpLen = (long) (pos - last);
                // machine code generated by nasm
                // cmp byte [rdx], 0
                // jne 0x1
                code.push_back(0x80);
                code.push_back(0x3a);
                code.push_back(0x00);
                code.push_back(0x0f);
                code.push_back(0x85);
                // the pos of the matching left bracket
                for (int i = 0; i < 4; i++) {
                    code.push_back((-jmpLen & ((unsigned long) 0xff << (8 * i))) >> (8 * i));
                }

                // set the pos of the matching left bracket
                assert(code[last + 5] == 0x12);
                assert(code[last + 6] == 0x34);
                assert(code[last + 7] == 0x56);
                assert(code[last + 8] == 0x78);
                for (int i = 0; i < 4; i++) {
                    code[last + 5 + i] = ((jmpLen & ((unsigned long) 0xff << (8 * i))) >> (8 * i));
                }
            }
                break;
            case COMMAND_TYPE::INPUT:
                // machine code generated by nasm
                // use the linux syscall `read`
                // asm reference: https://en.wikibooks.org/wiki/X86_Assembly/Interfacing_with_Linux#Making_a_system_call
                // syscall no definitions: https://android.googlesource.com/platform/prebuilts/gcc/linux-x86/host/x86_64-linux-glibc2.7-4.6/+/refs/heads/jb-dev/sysroot/usr/include/asm/unistd_64.h
                // `read` docs: https://man7.org/linux/man-pages/man2/read.2.html
                // mov rax, 0      ; `read` syscall no 0
                // mov rdi, 0      ; 1st arg: int fd, value 0 (stdin)
                // mov rsi, rdx    ; 2nd arg: void* buf, addr rdx
                // push rdx        ; save rdx to stack
                // mov rdx, 1      ; 3rd arg: size_t count, 1 character
                // syscall         ; do the syscall
                // pop rdx         ; restore rdx
                for (unsigned char ch: {0xb8, 0x00, 0x00, 0x00, 0x00, 0xbf, 0x00, 0x00, 0x00, 0x00, 0x48, 0x89, 0xd6, 0x52, 0xba, 0x01, 0x00, 0x00, 0x00, 0x0f, 0x05, 0x5a}) {
                    code.push_back(ch);
                }
                break;
            case COMMAND_TYPE::OUTPUT:
                // machine code generated by nasm
                // use the linux syscall `write`
                // `write` docs: https://man7.org/linux/man-pages/man2/write.2.html
                // mov rax, 1      ; `write` syscall no 1
                // mov rdi, 1      ; 1st arg: int fd, value 1 (stdout)
                // mov rsi, rdx    ; 2nd arg: void* buf, addr rdx
                // push rdx        ; save rdx to stack
                // mov rdx, 1      ; 3rd arg: size_t count, 1 character
                // syscall         ; do the syscall
                // pop rdx         ; restore rdx
                for (unsigned char ch: {0xb8, 0x01, 0x00, 0x00, 0x00, 0xbf, 0x01, 0x00, 0x00, 0x00, 0x48, 0x89, 0xd6, 0x52, 0xba, 0x01, 0x00, 0x00, 0x00, 0x0f, 0x05, 0x5a}) {
                    code.push_back(ch);
                }
                break;
            default: ;
                // ignore invalid characters, allow comments and linebreaks
//                assert(false);
        }
    }

    assert(bracketStack.empty());

    code.push_back(0xc3);  // machine code `ret` to exit the func

    return code;
}


int main(int argc, char *argv[]) {
    string line;

    if (argc == 2) {
        // read from the file specified by CLI args
        ifstream ifs;
        ifs.open(argv[1],ios::in);

        stringstream buffer;
        buffer << ifs.rdbuf();

        line = buffer.str();

        ifs.close();
    } else if (argc == 1) {
        cerr << "Warning: No program file specified, using the first line as BF program." << endl;
        // use the first line as the program (cannot use stdio in C, whose userspace cache breaks `read` syscall)
        char buf[1];
        do {
            read(0, buf, 1);
            line += buf[0];
        } while (buf[0] != '\n');
    } else {
        cerr << "Fatal: Invalid args count." << endl;
        cerr << "Usage: BrainfuckJIT file" << endl;
        return 0;
    }

    // translate from string to the custom command sequence
    vector<COMMAND*> commands = translate_command(line);

    // translate from the custom command sequence to asm machine code
    vector<unsigned char> code = translate_asm(commands, cells);

    // execute!
    execute_asm(code.data(), code.size());

    return 0;
}
