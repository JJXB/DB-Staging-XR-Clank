#pragma once
#include <array>
#include <cstdint>

class SectionProp;

namespace pix1000 {

struct Pal_WI01P006_U6 {
    static constexpr int NUM_INPUTS = 16;
    static constexpr int NUM_OUTPUTS = 5;
    static constexpr int NUM_REGS = 0;
    // input pins: [1, 2, 3, 4, 5, 6, 7, 8, 9, 11, 12, 13, 14, 17, 18, 19]
    // output pins: [15, 16, 17, 18, 19]
    // reg pins: []
    std::array<int,0> reg{};
    std::array<int,5> out{};
    std::array<int,5> oe{};
    void eval(const int* in) {
        std::array<int,0> nreg = reg;
        out[0] = (!((((!in[0]) && (!in[5])) || (in[0] && (!in[11])))));
        out[1] = (!((((!in[0]) && (!in[4])) || (in[0] && (!in[9])))));
        out[2] = (!((((!in[0]) && (!in[3])) || (in[0] && (!in[8])))));
        out[3] = (!((((!in[0]) && (!in[2])) || (in[0] && (!in[7])))));
        out[4] = (!((((!in[0]) && (!in[1])) || (in[0] && (!in[6])))));
        oe[0] = ((1));
        oe[1] = ((1));
        oe[2] = ((1));
        oe[3] = ((1));
        oe[4] = ((1));
        reg = nreg;
    }
};
struct Pal_WI01P026_U26 {
    static constexpr int NUM_INPUTS = 16;
    static constexpr int NUM_OUTPUTS = 5;
    static constexpr int NUM_REGS = 0;
    // input pins: [1, 2, 3, 4, 5, 6, 7, 8, 9, 11, 12, 13, 14, 17, 18, 19]
    // output pins: [15, 16, 17, 18, 19]
    // reg pins: []
    std::array<int,0> reg{};
    std::array<int,5> out{};
    std::array<int,5> oe{};
    void eval(const int* in) {
        std::array<int,0> nreg = reg;
        out[0] = (!((((!in[0]) && (!in[5])) || (in[0] && (!in[11])))));
        out[1] = (!((((!in[0]) && (!in[4])) || (in[0] && (!in[9])))));
        out[2] = (!((((!in[0]) && (!in[3])) || (in[0] && (!in[8])))));
        out[3] = (!((((!in[0]) && (!in[2])) || (in[0] && (!in[7])))));
        out[4] = (!((((!in[0]) && (!in[1])) || (in[0] && (!in[6])))));
        oe[0] = ((1));
        oe[1] = ((1));
        oe[2] = ((1));
        oe[3] = ((1));
        oe[4] = ((1));
        reg = nreg;
    }
};
struct Pal_WI01P044_U44 {
    static constexpr int NUM_INPUTS = 16;
    static constexpr int NUM_OUTPUTS = 5;
    static constexpr int NUM_REGS = 0;
    // input pins: [1, 2, 3, 4, 5, 6, 7, 8, 9, 11, 13, 14, 15, 16, 17, 18]
    // output pins: [13, 16, 17, 18, 19]
    // reg pins: []
    std::array<int,0> reg{};
    std::array<int,5> out{};
    std::array<int,5> oe{};
    void eval(const int* in) {
        std::array<int,0> nreg = reg;
        out[0] = (!((((!in[8]) && (!in[11])) || (in[8] && (!in[12])))));
        out[1] = (!((((!in[3]) && (!in[8])) || ((!in[7]) && in[8]))));
        out[2] = (!((((!in[2]) && (!in[8])) || ((!in[6]) && in[8]))));
        out[3] = (!((((!in[1]) && (!in[8])) || ((!in[5]) && in[8]))));
        out[4] = (!((((!in[0]) && (!in[8])) || ((!in[4]) && in[8]))));
        oe[0] = ((1));
        oe[1] = ((1));
        oe[2] = ((1));
        oe[3] = ((1));
        oe[4] = ((1));
        reg = nreg;
    }
};
struct Pal_WI01P045_U45 {
    static constexpr int NUM_INPUTS = 16;
    static constexpr int NUM_OUTPUTS = 8;
    static constexpr int NUM_REGS = 8;
    // input pins: [2, 3, 4, 5, 6, 7, 8, 9, 12, 13, 14, 15, 16, 17, 18, 19]
    // output pins: [12, 13, 14, 15, 16, 17, 18, 19]
    // reg pins: ['rf12', 'rf13', 'rf14', 'rf15', 'rf16', 'rf17', 'rf18', 'rf19']
    std::array<int,8> reg{};
    std::array<int,8> out{};
    std::array<int,8> oe{};
    void eval(const int* in) {
        std::array<int,8> nreg = reg;
        reg = nreg;
        out[0] = reg[0];
        out[1] = reg[1];
        out[2] = reg[2];
        out[3] = reg[3];
        out[4] = reg[4];
        out[5] = reg[5];
        out[6] = reg[6];
        out[7] = reg[7];
        oe[0] = 1;
        oe[1] = 1;
        oe[2] = 1;
        oe[3] = 1;
        oe[4] = 1;
        oe[5] = 1;
        oe[6] = 1;
        oe[7] = 1;
    }
};
struct Pal_WI01P046_U46 {
    static constexpr int NUM_INPUTS = 16;
    static constexpr int NUM_OUTPUTS = 8;
    static constexpr int NUM_REGS = 4;
    // input pins: [2, 3, 4, 5, 6, 7, 8, 9, 12, 13, 14, 15, 16, 17, 18, 19]
    // output pins: [12, 13, 14, 15, 16, 17, 18, 19]
    // reg pins: ['rf14', 'rf15', 'rf16', 'rf17']
    std::array<int,4> reg{};
    std::array<int,8> out{};
    std::array<int,8> oe{};
    void eval(const int* in) {
        std::array<int,4> nreg = reg;
        out[0] = (!((((!in[0]) && in[2] && (!in[6])) || ((!in[0]) && in[2] && (!in[5])) || ((!in[0]) && in[2] && (!in[4])))));
        out[1] = (!((((!in[0]) && in[2] && (!in[6])) || ((!in[0]) && in[2] && (!in[5])) || ((!in[0]) && in[2] && (!in[4])))));
        out[6] = (!(((in[0] && in[2] && (!in[6])) || (in[0] && in[2] && (!in[5])) || (in[0] && in[2] && (!in[4])))));
        out[7] = (!(((in[0] && in[2] && (!in[6])) || (in[0] && in[2] && (!in[5])) || (in[0] && in[2] && (!in[4])))));
        oe[0] = ((1));
        oe[1] = ((1));
        oe[6] = ((1));
        oe[7] = ((1));
        reg = nreg;
        out[2] = reg[0];
        out[3] = reg[1];
        out[4] = reg[2];
        out[5] = reg[3];
        oe[2] = 1;
        oe[3] = 1;
        oe[4] = 1;
        oe[5] = 1;
    }
};
struct Pal_WI01P048_U48 {
    static constexpr int NUM_INPUTS = 20;
    static constexpr int NUM_OUTPUTS = 8;
    static constexpr int NUM_REGS = 8;
    // input pins: [2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23]
    // output pins: [15, 16, 17, 18, 19, 20, 21, 22]
    // reg pins: ['rf15', 'rf16', 'rf17', 'rf18', 'rf19', 'rf20', 'rf21', 'rf22']
    std::array<int,8> reg{};
    std::array<int,8> out{};
    std::array<int,8> oe{};
    void eval(const int* in) {
        std::array<int,8> nreg = reg;
        reg = nreg;
        out[0] = reg[0];
        out[1] = reg[1];
        out[2] = reg[2];
        out[3] = reg[3];
        out[4] = reg[4];
        out[5] = reg[5];
        out[6] = reg[6];
        out[7] = reg[7];
        oe[0] = 1;
        oe[1] = 1;
        oe[2] = 1;
        oe[3] = 1;
        oe[4] = 1;
        oe[5] = 1;
        oe[6] = 1;
        oe[7] = 1;
    }
};
struct Pal_WI01P049_U49 {
    static constexpr int NUM_INPUTS = 20;
    static constexpr int NUM_OUTPUTS = 8;
    static constexpr int NUM_REGS = 8;
    // input pins: [2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23]
    // output pins: [15, 16, 17, 18, 19, 20, 21, 22]
    // reg pins: ['rf15', 'rf16', 'rf17', 'rf18', 'rf19', 'rf20', 'rf21', 'rf22']
    std::array<int,8> reg{};
    std::array<int,8> out{};
    std::array<int,8> oe{};
    void eval(const int* in) {
        std::array<int,8> nreg = reg;
        reg = nreg;
        out[0] = reg[0];
        out[1] = reg[1];
        out[2] = reg[2];
        out[3] = reg[3];
        out[4] = reg[4];
        out[5] = reg[5];
        out[6] = reg[6];
        out[7] = reg[7];
        oe[0] = 1;
        oe[1] = 1;
        oe[2] = 1;
        oe[3] = 1;
        oe[4] = 1;
        oe[5] = 1;
        oe[6] = 1;
        oe[7] = 1;
    }
};
struct Pal_WI01P050_U50 {
    static constexpr int NUM_INPUTS = 16;
    static constexpr int NUM_OUTPUTS = 7;
    static constexpr int NUM_REGS = 0;
    // input pins: [1, 2, 3, 4, 5, 6, 7, 8, 9, 11, 13, 14, 15, 16, 17, 18]
    // output pins: [13, 14, 15, 16, 17, 18, 19]
    // reg pins: []
    std::array<int,0> reg{};
    std::array<int,7> out{};
    std::array<int,7> oe{};
    void eval(const int* in) {
        std::array<int,0> nreg = reg;
        out[0] = (!((((!in[7])))));
        out[1] = (!((((!in[6])))));
        out[2] = (!((((!in[5])))));
        out[3] = (!((((!in[1]) && (!in[2]) && (!in[4])))));
        out[4] = (!(((in[1] && (!in[2]) && (!in[4])))));
        out[5] = (!((((!in[1]) && in[2] && (!in[4])))));
        out[6] = (!(((in[1] && in[2] && (!in[4])))));
        oe[0] = (((!in[8])));
        oe[1] = (((!in[8])));
        oe[2] = (((!in[8])));
        oe[3] = ((1));
        oe[4] = ((1));
        oe[5] = ((1));
        oe[6] = ((1));
        reg = nreg;
    }
};
struct Pal_WI01P053_U53 {
    static constexpr int NUM_INPUTS = 16;
    static constexpr int NUM_OUTPUTS = 8;
    static constexpr int NUM_REGS = 5;
    // input pins: [2, 3, 4, 5, 6, 7, 8, 9, 12, 13, 14, 15, 16, 17, 18, 19]
    // output pins: [12, 13, 14, 15, 16, 17, 18, 19]
    // reg pins: ['rf12', 'rf14', 'rf15', 'rf16', 'rf17']
    std::array<int,5> reg{};
    std::array<int,8> out{};
    std::array<int,8> oe{};
    void eval(const int* in) {
        std::array<int,5> nreg = reg;
        out[1] = (!((((!in[1]) && reg[2]))));
        out[6] = (((!in[0]) && (!in[3])) || (in[0] && reg[2] && (!reg[3]) && out[6]) || (reg[2] && (!reg[3]) && reg[4] && out[6]));
        out[7] = (!((((!in[6])) || (reg[2] && (!reg[3]) && (!reg[4])))));
        oe[1] = ((1));
        oe[6] = ((1));
        oe[7] = ((1));
        reg = nreg;
        out[0] = reg[0];
        out[2] = reg[1];
        out[3] = reg[2];
        out[4] = reg[3];
        out[5] = reg[4];
        oe[0] = 1;
        oe[2] = 1;
        oe[3] = 1;
        oe[4] = 1;
        oe[5] = 1;
    }
};
struct Pal_WI01P054_U54 {
    static constexpr int NUM_INPUTS = 16;
    static constexpr int NUM_OUTPUTS = 5;
    static constexpr int NUM_REGS = 0;
    // input pins: [1, 2, 3, 4, 5, 6, 7, 8, 9, 11, 12, 13, 14, 17, 18, 19]
    // output pins: [15, 16, 17, 18, 19]
    // reg pins: []
    std::array<int,0> reg{};
    std::array<int,5> out{};
    std::array<int,5> oe{};
    void eval(const int* in) {
        std::array<int,0> nreg = reg;
        out[0] = (0);
        out[1] = (!((((!in[3]) && (!in[8])) || ((!in[7]) && in[8]))));
        out[2] = (!((((!in[2]) && (!in[8])) || ((!in[6]) && in[8]))));
        out[3] = (!((((!in[1]) && (!in[8])) || ((!in[5]) && in[8]))));
        out[4] = (!((((!in[0]) && (!in[8])) || ((!in[4]) && in[8]))));
        oe[0] = ((1));
        oe[1] = ((1));
        oe[2] = ((1));
        oe[3] = ((1));
        oe[4] = ((1));
        reg = nreg;
    }
};
struct Pal_WI01P056_U56 {
    static constexpr int NUM_INPUTS = 16;
    static constexpr int NUM_OUTPUTS = 6;
    static constexpr int NUM_REGS = 6;
    // input pins: [2, 3, 4, 5, 6, 7, 8, 9, 12, 13, 14, 15, 16, 17, 18, 19]
    // output pins: [14, 15, 16, 17, 18, 19]
    // reg pins: ['rf14', 'rf15', 'rf16', 'rf17', 'rf18', 'rf19']
    std::array<int,6> reg{};
    std::array<int,6> out{};
    std::array<int,6> oe{};
    void eval(const int* in) {
        std::array<int,6> nreg = reg;
        reg = nreg;
        out[0] = reg[0];
        out[1] = reg[1];
        out[2] = reg[2];
        out[3] = reg[3];
        out[4] = reg[4];
        out[5] = reg[5];
        oe[0] = 1;
        oe[1] = 1;
        oe[2] = 1;
        oe[3] = 1;
        oe[4] = 1;
        oe[5] = 1;
    }
};
struct Pal_WI01P058_U58 {
    static constexpr int NUM_INPUTS = 16;
    static constexpr int NUM_OUTPUTS = 5;
    static constexpr int NUM_REGS = 5;
    // input pins: [2, 3, 4, 5, 6, 7, 8, 9, 12, 13, 14, 15, 16, 17, 18, 19]
    // output pins: [13, 14, 15, 16, 17]
    // reg pins: ['rf13', 'rf14', 'rf15', 'rf16', 'rf17']
    std::array<int,5> reg{};
    std::array<int,5> out{};
    std::array<int,5> oe{};
    void eval(const int* in) {
        std::array<int,5> nreg = reg;
        reg = nreg;
        out[0] = reg[0];
        out[1] = reg[1];
        out[2] = reg[2];
        out[3] = reg[3];
        out[4] = reg[4];
        oe[0] = 1;
        oe[1] = 1;
        oe[2] = 1;
        oe[3] = 1;
        oe[4] = 1;
    }
};
struct Pal_WI01P067_U67 {
    static constexpr int NUM_INPUTS = 16;
    static constexpr int NUM_OUTPUTS = 8;
    static constexpr int NUM_REGS = 4;
    // input pins: [2, 3, 4, 5, 6, 7, 8, 9, 12, 13, 14, 15, 16, 17, 18, 19]
    // output pins: [12, 13, 14, 15, 16, 17, 18, 19]
    // reg pins: ['rf14', 'rf15', 'rf16', 'rf17']
    std::array<int,4> reg{};
    std::array<int,8> out{};
    std::array<int,8> oe{};
    void eval(const int* in) {
        std::array<int,4> nreg = reg;
        out[0] = (!(((in[3] && (!reg[0])) || (in[2] && (!in[3])))));
        out[1] = (!(((in[3] && (!reg[1])) || (in[2] && (!in[3])))));
        out[6] = (!(((in[3] && (!reg[2])) || (in[2] && (!in[3])))));
        out[7] = (!(((in[3] && (!reg[3])) || (in[2] && (!in[3])))));
        oe[0] = ((1));
        oe[1] = ((1));
        oe[6] = ((1));
        oe[7] = ((1));
        reg = nreg;
        out[2] = reg[0];
        out[3] = reg[1];
        out[4] = reg[2];
        out[5] = reg[3];
        oe[2] = 1;
        oe[3] = 1;
        oe[4] = 1;
        oe[5] = 1;
    }
};
struct Pal_WI01P070_U70 {
    static constexpr int NUM_INPUTS = 20;
    static constexpr int NUM_OUTPUTS = 6;
    static constexpr int NUM_REGS = 4;
    // input pins: [2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23]
    // output pins: [15, 16, 17, 18, 19, 20]
    // reg pins: ['rf17', 'rf18', 'rf19', 'rf20']
    std::array<int,4> reg{};
    std::array<int,6> out{};
    std::array<int,6> oe{};
    void eval(const int* in) {
        std::array<int,4> nreg = reg;
        out[0] = (((!in[0]) && (!in[2]) && in[5] && in[6]) || ((!in[2]) && (!in[5]) && (!in[6]) && (!in[7]) && (!in[8]) && in[9] && (!in[10]) && in[17] && in[18] && (!in[19])));
        out[1] = (((!in[0]) && (!in[1]) && (!in[2]) && (!in[3]) && in[5] && in[6] && reg[3]) || ((!in[0]) && (!in[1]) && (!in[2]) && (!in[3]) && in[5] && in[6] && (!reg[2])) || ((!in[0]) && (!in[1]) && (!in[2]) && (!in[3]) && in[5] && in[6] && reg[1]) || ((!in[1]) && (!in[2]) && (!in[3]) && (!in[5]) && (!in[6]) && (!in[7]) && (!in[8]) && in[9] && (!in[10]) && reg[3] && in[17] && in[18] && (!in[19])) || ((!in[1]) && (!in[2]) && (!in[3]) && (!in[5]) && (!in[6]) && (!in[7]) && (!in[8]) && in[9] && (!in[10]) && (!reg[2]) && in[17] && in[18] && (!in[19])) || ((!in[1]) && (!in[2]) && (!in[3]) && (!in[5]) && (!in[6]) && (!in[7]) && (!in[8]) && in[9] && (!in[10]) && reg[1] && in[17] && in[18] && (!in[19])));
        oe[0] = ((1));
        oe[1] = ((1));
        reg = nreg;
        out[2] = reg[0];
        out[3] = reg[1];
        out[4] = reg[2];
        out[5] = reg[3];
        oe[2] = 1;
        oe[3] = 1;
        oe[4] = 1;
        oe[5] = 1;
    }
};
struct Pal_WI01P072_U72 {
    static constexpr int NUM_INPUTS = 20;
    static constexpr int NUM_OUTPUTS = 8;
    static constexpr int NUM_REGS = 2;
    // input pins: [2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23]
    // output pins: [15, 16, 17, 18, 19, 20, 21, 22]
    // reg pins: ['rf15', 'rf16']
    std::array<int,2> reg{};
    std::array<int,8> out{};
    std::array<int,8> oe{};
    void eval(const int* in) {
        std::array<int,2> nreg = reg;
        out[2] = (!(((in[6] && (!in[7])))));
        out[3] = (!(((in[5] && (!in[7])))));
        out[4] = ((in[0] && (!in[4]) && (!in[8])) || (in[0] && (!in[4]) && in[9]));
        out[5] = ((in[1] && (!in[4]) && (!in[8])) || (in[1] && (!in[4]) && in[9]));
        out[6] = ((in[2] && (!in[4]) && (!in[8])) || (in[2] && (!in[4]) && in[9]));
        out[7] = ((in[3] && (!in[4]) && (!in[8])) || (in[3] && (!in[4]) && in[9]));
        oe[2] = ((1));
        oe[3] = ((1));
        oe[4] = ((1));
        oe[5] = ((1));
        oe[6] = ((1));
        oe[7] = ((1));
        reg = nreg;
        out[0] = reg[0];
        out[1] = reg[1];
        oe[0] = 1;
        oe[1] = 1;
    }
};
struct Pal_WI01P073_U73 {
    static constexpr int NUM_INPUTS = 20;
    static constexpr int NUM_OUTPUTS = 7;
    static constexpr int NUM_REGS = 0;
    // input pins: [1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 14, 15, 16, 17, 20, 21, 22, 23]
    // output pins: [15, 17, 18, 19, 20, 21, 22]
    // reg pins: []
    std::array<int,0> reg{};
    std::array<int,7> out{};
    std::array<int,7> oe{};
    void eval(const int* in) {
        std::array<int,0> nreg = reg;
        out[0] = (!(((in[0] && in[7] && in[8] && in[9] && (!in[19])))));
        out[1] = (!((0)));
        out[2] = (!((0)));
        out[3] = (!(((in[1] && (!in[4]) && in[5] && (!in[6]) && (!in[7]) && in[12]) || (in[7] && in[8] && in[12] && (!in[19])) || (in[1] && (!in[3]) && (!in[6]) && (!in[7]) && in[12]) || ((!in[2]) && in[3] && (!in[6]) && (!in[7]) && in[12]) || (in[7] && in[8] && in[10] && (!in[19])))));
        out[4] = (!(((in[1] && (!in[4]) && in[5] && (!in[6]) && (!in[7]) && (!in[12])) || (in[7] && in[8] && (!in[12]) && (!in[19])) || (in[1] && (!in[3]) && (!in[6]) && (!in[7]) && (!in[12])) || ((!in[2]) && in[3] && (!in[6]) && (!in[7]) && (!in[12])) || (in[7] && in[8] && in[10] && (!in[19])))));
        out[5] = (!(((in[0] && in[7] && in[12] && (!in[19])) || (in[0] && in[1] && (!in[4]) && in[5] && in[6] && (!in[7]) && in[12]) || (in[0] && (!in[2]) && (!in[4]) && in[5] && in[6] && (!in[7]) && in[12]) || (in[0] && in[7] && in[10] && (!in[19])) || (in[0] && in[1] && (!in[3]) && in[6] && (!in[7]) && in[12]) || (in[0] && (!in[2]) && in[3] && in[6] && (!in[7]) && in[12]))));
        out[6] = (!(((in[0] && in[7] && (!in[12]) && (!in[19])) || (in[0] && in[1] && (!in[4]) && in[5] && in[6] && (!in[7]) && (!in[12])) || (in[0] && (!in[2]) && (!in[4]) && in[5] && in[6] && (!in[7]) && (!in[12])) || (in[0] && in[7] && in[10] && (!in[19])) || (in[0] && in[1] && (!in[3]) && in[6] && (!in[7]) && (!in[12])) || (in[0] && (!in[2]) && in[3] && in[6] && (!in[7]) && (!in[12])))));
        oe[0] = ((1));
        oe[1] = ((1));
        oe[2] = ((1));
        oe[3] = ((1));
        oe[4] = ((1));
        oe[5] = ((1));
        oe[6] = ((1));
        reg = nreg;
    }
};
struct Pal_WI01P074_U74 {
    static constexpr int NUM_INPUTS = 20;
    static constexpr int NUM_OUTPUTS = 8;
    static constexpr int NUM_REGS = 8;
    // input pins: [2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23]
    // output pins: [15, 16, 17, 18, 19, 20, 21, 22]
    // reg pins: ['rf15', 'rf16', 'rf17', 'rf18', 'rf19', 'rf20', 'rf21', 'rf22']
    std::array<int,8> reg{};
    std::array<int,8> out{};
    std::array<int,8> oe{};
    void eval(const int* in) {
        std::array<int,8> nreg = reg;
        reg = nreg;
        out[0] = reg[0];
        out[1] = reg[1];
        out[2] = reg[2];
        out[3] = reg[3];
        out[4] = reg[4];
        out[5] = reg[5];
        out[6] = reg[6];
        out[7] = reg[7];
        oe[0] = 1;
        oe[1] = 1;
        oe[2] = 1;
        oe[3] = 1;
        oe[4] = 1;
        oe[5] = 1;
        oe[6] = 1;
        oe[7] = 1;
    }
};
struct Pal_WI01P075_U75 {
    static constexpr int NUM_INPUTS = 16;
    static constexpr int NUM_OUTPUTS = 4;
    static constexpr int NUM_REGS = 4;
    // input pins: [2, 3, 4, 5, 6, 7, 8, 9, 12, 13, 14, 15, 16, 17, 18, 19]
    // output pins: [14, 15, 16, 17]
    // reg pins: ['rf14', 'rf15', 'rf16', 'rf17']
    std::array<int,4> reg{};
    std::array<int,4> out{};
    std::array<int,4> oe{};
    void eval(const int* in) {
        std::array<int,4> nreg = reg;
        reg = nreg;
        out[0] = reg[0];
        out[1] = reg[1];
        out[2] = reg[2];
        out[3] = reg[3];
        oe[0] = 1;
        oe[1] = 1;
        oe[2] = 1;
        oe[3] = 1;
    }
};
struct Pal_WI01P079_U79 {
    static constexpr int NUM_INPUTS = 16;
    static constexpr int NUM_OUTPUTS = 3;
    static constexpr int NUM_REGS = 2;
    // input pins: [2, 3, 4, 5, 6, 7, 8, 9, 12, 13, 14, 15, 16, 17, 18, 19]
    // output pins: [17, 18, 19]
    // reg pins: ['rf17', 'rf18']
    std::array<int,2> reg{};
    std::array<int,3> out{};
    std::array<int,3> oe{};
    void eval(const int* in) {
        std::array<int,2> nreg = reg;
        out[2] = (!(((in[2] && (!in[5]) && in[6]))));
        oe[2] = ((1));
        reg = nreg;
        out[0] = reg[0];
        out[1] = reg[1];
        oe[0] = 1;
        oe[1] = 1;
    }
};
struct Pal_WI01P080_U80 {
    static constexpr int NUM_INPUTS = 16;
    static constexpr int NUM_OUTPUTS = 7;
    static constexpr int NUM_REGS = 4;
    // input pins: [2, 3, 4, 5, 6, 7, 8, 9, 12, 13, 14, 15, 16, 17, 18, 19]
    // output pins: [12, 14, 15, 16, 17, 18, 19]
    // reg pins: ['rf14', 'rf15', 'rf16', 'rf17']
    std::array<int,4> reg{};
    std::array<int,7> out{};
    std::array<int,7> oe{};
    void eval(const int* in) {
        std::array<int,4> nreg = reg;
        out[0] = (!((((!in[0]) && (!in[7])) || ((!in[0]) && in[5] && (!in[6]) && (!reg[0]) && (!reg[1]) && (!reg[2]) && (!reg[3])))));
        out[5] = (!((((!in[7])) || (in[6]) || ((!reg[0]) && (!reg[1]) && (!reg[2]) && (!reg[3])))));
        out[6] = (!((((!in[0]) && (!in[6]) && (!reg[0]) && (!reg[1]) && (!reg[2]) && (!reg[3])))));
        oe[0] = ((1));
        oe[5] = ((1));
        oe[6] = ((1));
        reg = nreg;
        out[1] = reg[0];
        out[2] = reg[1];
        out[3] = reg[2];
        out[4] = reg[3];
        oe[1] = 1;
        oe[2] = 1;
        oe[3] = 1;
        oe[4] = 1;
    }
};
struct Pal_WI01P083_U83 {
    static constexpr int NUM_INPUTS = 20;
    static constexpr int NUM_OUTPUTS = 8;
    static constexpr int NUM_REGS = 8;
    // input pins: [2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23]
    // output pins: [15, 16, 17, 18, 19, 20, 21, 22]
    // reg pins: ['rf15', 'rf16', 'rf17', 'rf18', 'rf19', 'rf20', 'rf21', 'rf22']
    std::array<int,8> reg{};
    std::array<int,8> out{};
    std::array<int,8> oe{};
    void eval(const int* in) {
        std::array<int,8> nreg = reg;
        reg = nreg;
        out[0] = reg[0];
        out[1] = reg[1];
        out[2] = reg[2];
        out[3] = reg[3];
        out[4] = reg[4];
        out[5] = reg[5];
        out[6] = reg[6];
        out[7] = reg[7];
        oe[0] = 1;
        oe[1] = 1;
        oe[2] = 1;
        oe[3] = 1;
        oe[4] = 1;
        oe[5] = 1;
        oe[6] = 1;
        oe[7] = 1;
    }
};
struct Pal_WI01P092_U92 {
    static constexpr int NUM_INPUTS = 20;
    static constexpr int NUM_OUTPUTS = 8;
    static constexpr int NUM_REGS = 8;
    // input pins: [2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23]
    // output pins: [15, 16, 17, 18, 19, 20, 21, 22]
    // reg pins: ['rf15', 'rf16', 'rf17', 'rf18', 'rf19', 'rf20', 'rf21', 'rf22']
    std::array<int,8> reg{};
    std::array<int,8> out{};
    std::array<int,8> oe{};
    void eval(const int* in) {
        std::array<int,8> nreg = reg;
        reg = nreg;
        out[0] = reg[0];
        out[1] = reg[1];
        out[2] = reg[2];
        out[3] = reg[3];
        out[4] = reg[4];
        out[5] = reg[5];
        out[6] = reg[6];
        out[7] = reg[7];
        oe[0] = 1;
        oe[1] = 1;
        oe[2] = 1;
        oe[3] = 1;
        oe[4] = 1;
        oe[5] = 1;
        oe[6] = 1;
        oe[7] = 1;
    }
};
} // namespace pix1000

// Config + install/uninstall entry points (defined in pix1000.cpp).
void SU2000_PIX1000_AddConfig(SectionProp& section);
bool  SU2000_PIX1000_Install(const SectionProp& section);
void  SU2000_PIX1000_Uninstall();
bool  SU2000_PIX1000_Installed();
