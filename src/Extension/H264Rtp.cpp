﻿/*
 * Copyright (c) 2016 The ZLMediaKit project authors. All Rights Reserved.
 *
 * This file is part of ZLMediaKit(https://github.com/xia-chu/ZLMediaKit).
 *
 * Use of this source code is governed by MIT license that can be found in the
 * LICENSE file in the root of the source tree. All contributing project authors
 * may be found in the AUTHORS file in the root of the source tree.
 */

#include "H264Rtp.h"

namespace mediakit{

#if defined(_WIN32)
#pragma pack(push, 1)
#endif // defined(_WIN32)

class FuFlags {
public:
#if __BYTE_ORDER == __BIG_ENDIAN
    unsigned start_bit: 1;
    unsigned end_bit: 1;
    unsigned reserved: 1;
    unsigned nal_type: 5;
#else
    unsigned nal_type: 5;
    unsigned reserved: 1;
    unsigned end_bit: 1;
    unsigned start_bit: 1;
#endif
} PACKED;

#if defined(_WIN32)
#pragma pack(pop)
#endif // defined(_WIN32)

H264RtpDecoder::H264RtpDecoder() {
    _frame = obtainFrame();
}

H264Frame::Ptr H264RtpDecoder::obtainFrame() {
    auto frame = FrameImp::create<H264Frame>();
    frame->_prefix_size = 4;
    return frame;
}

bool H264RtpDecoder::inputRtp(const RtpPacket::Ptr &rtp, bool key_pos) {
    return decodeRtp(rtp);
}

/*
RTF3984 5.2节  Common Structure of the RTP Payload Format
Table 1.  Summary of NAL unit types and their payload structures

   Type   Packet    Type name                        Section
   ---------------------------------------------------------
   0      undefined                                    -
   1-23   NAL unit  Single NAL unit packet per H.264   5.6
   24     STAP-A    Single-time aggregation packet     5.7.1
   25     STAP-B    Single-time aggregation packet     5.7.1
   26     MTAP16    Multi-time aggregation packet      5.7.2
   27     MTAP24    Multi-time aggregation packet      5.7.2
   28     FU-A      Fragmentation unit                 5.8
   29     FU-B      Fragmentation unit                 5.8
   30-31  undefined                                    -
*/

bool H264RtpDecoder::decodeRtp(const RtpPacket::Ptr &rtp) {
    auto frame = rtp->getPayload();
    auto length = rtp->getPayloadSize();
    auto stamp = rtp->getStampMS();
    auto seq = rtp->getSeq();
    auto nal_type = *frame & 0x1F;
    auto nal_suffix = *frame & (~0x1F);

    if (nal_type >= 0 && nal_type < 24) {
        //a full frame
        _frame->_buffer.assign("\x00\x00\x00\x01", 4);
        _frame->_buffer.append((char *) frame, length);
        _frame->_pts = stamp;
        auto key = _frame->keyFrame();
        onGetH264(_frame);
        return (key); //i frame
    }

    switch (nal_type) {
        case 24: {
            // 24 STAP-A   单一时间的组合包
            bool haveIDR = false;
            auto ptr = frame + 1;
            while (true) {
                size_t off = ptr - frame;
                if (off >= length) {
                    break;
                }
                //获取当前nalu的大小
                uint16_t len = *ptr++;
                len <<= 8;
                len |= *ptr++;
                if (off + len > length) {
                    break;
                }
                if (len > 0) {
                    //有有效数据
                    _frame->_buffer.assign("\x00\x00\x00\x01", 4);
                    _frame->_buffer.append((char *) ptr, len);
                    _frame->_pts = stamp;
                    if ((ptr[0] & 0x1F) == H264Frame::NAL_IDR) {
                        haveIDR = true;
                    }
                    onGetH264(_frame);
                }
                ptr += len;
            }
            return haveIDR;
        }

        case 28: {
            //FU-A
            FuFlags *fu = (FuFlags *) (frame + 1);
            if (fu->start_bit) {
                //该帧的第一个rtp包  FU-A start
                //预留空间，防止频繁扩容拷贝
                _frame->_buffer.reserve(_max_frame_size);
                _frame->_buffer.assign("\x00\x00\x00\x01", 4);
                _frame->_buffer.push_back(nal_suffix | fu->nal_type);
                _frame->_buffer.append((char *) frame + 2, length - 2);
                _frame->_pts = stamp;
                //该函数return时，保存下当前sequence,以便下次对比seq是否连续
                _last_seq = seq;
                return _frame->keyFrame();
            }

            if (seq != (uint16_t) (_last_seq + 1)) {
                //中间的或末尾的rtp包，其seq必须连续(如果回环了则判定为连续)，否则说明rtp丢包，那么该帧不完整，必须得丢弃
                _frame->_buffer.clear();
                WarnL << "rtp丢包: " << seq << " != " << _last_seq << " + 1,该帧被废弃";
                return false;
            }

            if (!fu->end_bit) {
                //该帧的中间rtp包  FU-A mid
                _frame->_buffer.append((char *) frame + 2, length - 2);
                //该函数return时，保存下当前sequence,以便下次对比seq是否连续
                _last_seq = seq;
                return false;
            }

            //该帧最后一个rtp包  FU-A end
            _frame->_buffer.append((char *) frame + 2, length - 2);
            _frame->_pts = stamp;
            //计算最大的帧
            auto frame_size = _frame->size();
            if (frame_size > _max_frame_size) {
                _max_frame_size = frame_size;
            }
            onGetH264(_frame);
            return false;
        }

        default: {
            // 29 FU-B     单NAL单元B模式
            // 25 STAP-B   单一时间的组合包
            // 26 MTAP16   多个时间的组合包
            // 27 MTAP24   多个时间的组合包
            WarnL << "不支持的rtp类型:" << (int) nal_type << " " << seq;
            return false;
        }
    }
}

void H264RtpDecoder::onGetH264(const H264Frame::Ptr &frame) {
    //rtsp没有dts，那么根据pts排序算法生成dts
    _dts_generator.getDts(frame->_pts,frame->_dts);
    RtpCodec::inputFrame(frame);
    _frame = obtainFrame();
}


////////////////////////////////////////////////////////////////////////

H264RtpEncoder::H264RtpEncoder(uint32_t ssrc, uint32_t mtu, uint32_t sample_rate, uint8_t pt, uint8_t interleaved)
        : RtpInfo(ssrc, mtu, sample_rate, pt, interleaved) {
}

void H264RtpEncoder::inputFrame(const Frame::Ptr &frame) {
    auto ptr = frame->data() + frame->prefixSize();
    auto len = frame->size() - frame->prefixSize();
    auto pts = frame->pts();
    auto nal_type = H264_TYPE(ptr[0]);
    auto packet_size = getMaxSize() - 2;

    //末尾5bit为nalu type，固定为28(FU-A)
    auto fu_char_0 = (ptr[0] & (~0x1F)) | 28;
    auto fu_char_1 = nal_type;
    FuFlags *fu_flags = (FuFlags *) (&fu_char_1);
    fu_flags->start_bit = 1;

    //超过MTU则按照FU-A模式打包
    if (len > packet_size + 1) {
        size_t offset = 1;
        while (!fu_flags->end_bit) {
            if (!fu_flags->start_bit && len <= offset + packet_size) {
                //FU-A end
                packet_size = len - offset;
                fu_flags->end_bit = 1;
            }

            //传入nullptr先不做payload的内存拷贝
            auto rtp = makeRtp(getTrackType(), nullptr, packet_size + 2, fu_flags->end_bit, pts);
            //rtp payload 负载部分
            uint8_t *payload = rtp->getPayload();
            //FU-A 第1个字节
            payload[0] = fu_char_0;
            //FU-A 第2个字节
            payload[1] = fu_char_1;
            //H264 数据
            memcpy(payload + 2, (uint8_t *) ptr + offset, packet_size);
            //输入到rtp环形缓存
            RtpCodec::inputRtp(rtp, fu_flags->start_bit && nal_type == H264Frame::NAL_IDR);

            offset += packet_size;
            fu_flags->start_bit = 0;
        }
    } else {
        //如果帧长度不超过mtu, 则按照Single NAL unit packet per H.264 方式打包
        makeH264Rtp(ptr, len, false, false, pts);
    }
}

void H264RtpEncoder::makeH264Rtp(const void* data, size_t len, bool mark, bool gop_pos, uint32_t uiStamp) {
    RtpCodec::inputRtp(makeRtp(getTrackType(), data, len, mark, uiStamp), gop_pos);
}

}//namespace mediakit
