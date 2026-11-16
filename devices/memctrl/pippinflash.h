/* Pippin Am29F010 save flash. GPL-3.0-or-later, matching DingusPPC.
 * Command sequences follow AMD Am29F010; Pippin's data bus reverses D0..D7
 * and connects flash A0 to CPU A2 (one byte on each 32-bit word's high lane).
 */
#pragma once
#include <array>
#include <algorithm>
#include <cstdint>
#include <fstream>
#include <string>
#include <stdexcept>
#include <cstdio>
#include <filesystem>

class PippinFlash {
public:
    static constexpr uint32_t capacity = 128 * 1024;
    explicit PippinFlash(std::string file = "pippin-flash.bin") : file_(std::move(file)) {
        data_.fill(0xff);
        std::ifstream in(file_, std::ios::binary);
        if (!in && std::filesystem::exists(file_)) throw std::runtime_error("Could not open existing Pippin flash backing file");
        if (in) {
            in.seekg(0, std::ios::end);
            if (in.tellg() != capacity) throw std::runtime_error("Invalid Pippin flash backing-file size");
            in.seekg(0); in.read(reinterpret_cast<char*>(data_.data()), capacity);
            if (!in) throw std::runtime_error("Could not read Pippin flash backing file");
        }
    }
    ~PippinFlash() { if (dirty_ && !save()) std::fprintf(stderr,"Could not save private Pippin flash: %s\n",file_.c_str()); }
    bool save() {
        if (!dirty_) return true;
        std::string tmp = file_ + ".tmp";
        { std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
          if (!out || !out.write(reinterpret_cast<const char*>(data_.data()), capacity)) return false;
          out.flush(); if (!out) return false; }
        if (std::rename(tmp.c_str(), file_.c_str())) return false;
        dirty_ = false; return true;
    }
    uint8_t read(uint32_t addr) const {
        addr &= capacity-1;
        if (state_ == State::Id) {
            switch (addr & 0xff) { case 0: return 0x01; case 1: return 0x20; default: return 0; }
        }
        return data_[addr];
    }
    void write(uint32_t addr, uint8_t value) {
        addr &= capacity-1;
        if (state_ == State::Program) {
            data_[addr] &= value; dirty_ = true; state_ = State::Read; return;
        }
        if (value == 0xf0) { state_ = State::Read; return; }
        switch (state_) {
        case State::Read: case State::Id:
            if (unlock1(addr) && value == 0xaa) state_ = State::Unlock;
            break;
        case State::Unlock:
            state_ = unlock2(addr) && value == 0x55 ? State::Command : State::Read; break;
        case State::Command:
            state_ = State::Read;
            if (unlock1(addr)) {
                if (value == 0x90) state_ = State::Id;
                else if (value == 0xa0) state_ = State::Program;
                else if (value == 0x80) state_ = State::EraseUnlock;
            }
            break;
        case State::EraseUnlock:
            state_ = unlock1(addr) && value == 0xaa ? State::EraseCommand : State::Read; break;
        case State::EraseCommand:
            state_ = unlock2(addr) && value == 0x55 ? State::Erase : State::Read; break;
        case State::Erase:
            if (value == 0x30) {
                auto begin = data_.begin() + (addr & ~0x3fffU);
                std::fill(begin, begin + 0x4000, 0xff); dirty_ = true;
            } else if (unlock1(addr) && value == 0x10) { data_.fill(0xff); dirty_ = true; }
            state_ = State::Read; break;
        default: state_ = State::Read; break;
        }
    }
    static uint8_t reverse(uint8_t v) {
        v = ((v & 0x55) << 1) | ((v >> 1) & 0x55);
        v = ((v & 0x33) << 2) | ((v >> 2) & 0x33);
        return (v << 4) | (v >> 4);
    }
    uint32_t bus_read(uint32_t offset, int size) const {
        uint32_t result = 0;
        for (int i=0;i<size;i++) {
            uint32_t a=offset+i;
            result=(result<<8) | ((a&3)==0 ? reverse(read(a>>2)) : 0xff);
        }
        return result;
    }
    void bus_write(uint32_t offset, uint32_t value, int size) {
        for (int i=0;i<size;i++) {
            uint32_t a=offset+i;
            if ((a&3)==0) write(a>>2, reverse(uint8_t(value >> (8*(size-1-i)))));
        }
    }
private:
    enum class State { Read, Unlock, Command, Id, Program, EraseUnlock, EraseCommand, Erase };
    static bool unlock1(uint32_t a) { return (a&0x7ff)==0x555; }
    static bool unlock2(uint32_t a) { return (a&0x7ff)==0x2aa; }
    std::array<uint8_t,capacity> data_;
    std::string file_;
    State state_ = State::Read;
    bool dirty_ = false;
};
