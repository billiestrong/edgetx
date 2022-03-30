#include "sumd.h"
#include "opentx.h"
#include "trainer.h"
#include "switches.h"

#include <algorithm>
#include <limits>

#ifdef __GNUC__
# pragma GCC diagnostic push
# pragma GCC diagnostic error "-Wswitch" // unfortunately the project uses -Wnoswitch
#endif

namespace SumDV3 {
    struct Crc16 {
        void reset() {
            sum = 0;
        }
        void operator+=(const uint8_t v) {
            sum = sum ^ (((uint16_t)v) << 8);
            for(uint8_t i = 0; i < 8; ++i) {
                if (sum & 0x8000) {
                    sum = (sum << 1) ^ crc_polynome;
                }
                else {
                    sum = (sum << 1);
                }
            }
        }
        operator uint16_t() const {
            return sum;
        }
    private:
        static constexpr uint16_t crc_polynome = 0x1021;
        uint16_t sum{};
    };
    
    struct Servo {
        using SumDV3 = Trainer::Protocol::SumDV3;
        using MesgType = SumDV3::MesgType;
        using SwitchesType = SumDV3::SwitchesType;
        
        enum class State : uint8_t {Undefined, GotStart, StartV1, StartV3, V1ChannelDataH, V1ChannelDataL, CrcH, CrcL,
                                   V3ChannelDataH, V3ChannelDataL, V3FuncCode, V3LastValidPackage, V3ModeCmd, V3SubCmd};
        
        enum class Frame : uint8_t {Ch1to12 = 0x00, First = Ch1to12, 
                                    Ch1to8and13to16 = 0x01, Ch1to16 = 0x02, Ch1to8and17to24 = 0x03,
                                    Ch1to8and25to32 = 0x04, Ch1to12and64Switches = 0x05,
                                    Last = Ch1to12and64Switches,
                                    Undefined = 0xff};
                                    
        
        static inline int16_t convertSumdToPuls(uint16_t const sumdValue) {
            return Trainer::clamp(sumdValue - SumDV3::CenterValue);
        }
        
        static inline void process(const uint8_t b, const std::function<void()> f) {
            switch(mState) { // enum-switch -> no default (intentional)
            case State::Undefined:
                if (b == SumDV3::start_code) {
                    csum.reset();
                    csum += b;
                    mState = State::GotStart;
                }
                break;
            case State::GotStart:
                csum += b;
                if ((b & 0x0f) == SumDV3::version_code1) {
                    mState = State::StartV1;
                }
                else if ((b & 0x0f) == SumDV3::version_code3) {
                    mState = State::StartV3;
                }
                else {
                    mState = State::Undefined;
                }
                break;
            case State::StartV1:
                if ((b >= 2) && (b <= 32)) {
                    csum += b;
                    nChannels = b;
                    mIndex = 0;
                    mState = State::V1ChannelDataH;
                }
                else {
                    mState = State::Undefined;
                }
                break;
            case State::V1ChannelDataH:
                csum += b;
                sumdFrame[mIndex].first = b;
                mState = State::V1ChannelDataL;
                break;
            case State::V1ChannelDataL:
                csum += b;
                sumdFrame[mIndex].second = b;
                ++mIndex;
                if (mIndex < nChannels) {
                    mState = State::V1ChannelDataH;
                }
                else {
                    mState = State::CrcH;
                    frame = Frame::Ch1to16;
                }
                break;
            case State::CrcH:
                crcH = b;
                mState = State::CrcL;
                break;
            case State::CrcL:
                if ((((uint16_t)crcH << 8) | b) == csum) {
                    ++mPackagesCounter;
                    f();
                }
                mState = State::Undefined;
                break;
            case State::StartV3:
                if ((b >= 2) && (b <= 68)) {
                    csum += b;
                    nChannels = b - 2;
                    mIndex = 0;
                    mState = State::V3ChannelDataH;
                }
                else {
                    mState = State::Undefined;
                }
                break;
            case State::V3ChannelDataH:
                csum += b;
                sumdFrame[mIndex].first = b;
                mState = State::V3ChannelDataL;
                break;
            case State::V3ChannelDataL:
                csum += b;
                sumdFrame[mIndex].second = b;
                ++mIndex;
                if (mIndex < nChannels) {
                    mState = State::V3ChannelDataH;
                }
                else {
                    mState = State::V3FuncCode;
                }
                break;
            case State::V3FuncCode:
                csum += b;
                if ((b >= uint8_t(Frame::First)) && (b <= uint8_t(Frame::Last))) {
                    frame = Frame(b);
                }
                else {
                    frame = Frame::Undefined;
                }
                mState = State::V3LastValidPackage;
                break;
            case State::V3LastValidPackage:
                csum += b;
                mState = State::V3ModeCmd;
                break;
            case State::V3ModeCmd:
                csum += b;
                mState = State::V3SubCmd;
                break;
            case State::V3SubCmd:
                csum += b;
                mState = State::CrcH;
                break;
            }            
        }        

        template<uint8_t B, uint8_t E> struct range_t {};

        template<uint8_t N>
        using offset_t = std::integral_constant<uint8_t, N>;
        
        template<uint8_t Begin, uint8_t End, uint8_t N, uint8_t Off = 0>
        static inline void extract(const range_t<Begin, End>&, int16_t (&dest)[N], offset_t<Off> = offset_t<0>{}) {
            static_assert((End - Begin) < (N - Off), "wrong range or target size");
            
        }
        
        static inline void sumSwitches() {
            uint64_t sw = sumdFrame[12].first;
            sw = (sw << 8) | sumdFrame[12].second;
            sw = (sw << 8) | sumdFrame[13].first;
            sw = (sw << 8) | sumdFrame[13].second;
            sw = (sw << 8) | sumdFrame[14].first;
            sw = (sw << 8) | sumdFrame[14].second;
            sw = (sw << 8) | sumdFrame[15].first;
            sw = (sw << 8) | sumdFrame[15].second;
            
            for (uint8_t i = 0; i < MAX_LOGICAL_SWITCHES; ++i) {
                const uint64_t mask = (1 << i);
                if (sw & mask) {
                    rawSetUnconnectedStickySwitch(i, true);
                }
                else {
                    rawSetUnconnectedStickySwitch(i, false);
                }
            }
        }
        
        template<uint8_t N>
        static inline void convert(int16_t (&pulses)[N]) {
            switch(frame) {
            case Frame::Ch1to12:
                extract(range_t<0, 11>{}, pulses);
                break;
            case Frame::Ch1to8and13to16:
                extract(range_t<0, 7>{}, pulses);
                extract(range_t<8, 11>{}, pulses, offset_t<12>{});
                break;
            case Frame::Ch1to8and17to24:
                extract(range_t<0, 7>{}, pulses);
                if constexpr(N > 16) {
                    extract(range_t<8, 15>{}, pulses, offset_t<16>{});
                }
                break;
            case Frame::Ch1to8and25to32:
                extract(range_t<0, 7>{}, pulses);
                if constexpr(N > 16) {
                    extract(range_t<8, 15>{}, pulses, offset_t<24>{});
                }
                break;
            case Frame::Ch1to16:
                extract(range_t<0, 15>{}, pulses);
                break;
            case Frame::Ch1to12and64Switches:
                extract(range_t<0, 11>{}, pulses);
                sumSwitches();
                break;
            case Frame::Undefined:
                break;
            }            
        }
    private:
        static uint8_t nChannels;
        static Crc16 csum;
        static uint8_t crcH;
        
        static State mState;
        static MesgType sumdFrame;
        static uint8_t mIndex;
        static uint16_t mPackagesCounter;

        static Frame frame;
        static uint8_t reserved;
        static uint8_t mode_cmd;
        static uint8_t sub_cmd;
    };
    uint8_t Servo::nChannels{};
    Crc16 Servo::csum{};
    uint8_t Servo::crcH{};
    
    Servo::State Servo::mState{Servo::State::Undefined};
    Servo::MesgType Servo::sumdFrame; 
    uint8_t Servo::mIndex{};
    uint16_t Servo::mPackagesCounter{};

    Servo::Frame Servo::frame{Servo::Frame::Undefined};
    uint8_t Servo::reserved{};
    uint8_t Servo::mode_cmd{};
    uint8_t Servo::sub_cmd{};
}

void processSumdInput() {
#if !defined(SIMU)
  uint8_t rxchar;

  while (sbusAuxGetByte(&rxchar)) {
      SumDV3::Servo::process(rxchar, [&](){
          SumDV3::Servo::convert(ppmInput);
          ppmInputValidityTimer = PPM_IN_VALID_TIMEOUT;        
      });
  }
#endif
}

#ifdef __GNUC__
# pragma GCC diagnostic pop
#endif
