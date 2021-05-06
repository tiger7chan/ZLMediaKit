﻿/*
 * Copyright (c) 2016 The ZLMediaKit project authors. All Rights Reserved.
 *
 * This file is part of ZLMediaKit(https://github.com/xia-chu/ZLMediaKit).
 *
 * Use of this source code is governed by MIT license that can be found in the
 * LICENSE file in the root of the source tree. All contributing project authors
 * may be found in the AUTHORS file in the root of the source tree.
 */

#include "RtcpContext.h"
#include "Util/logger.h"
using namespace toolkit;

namespace mediakit {

void RtcpContext::clear() {
    memset(this, 0, sizeof(RtcpContext));
}

RtcpContext::RtcpContext(uint32_t sample_rate, bool is_receiver) {
    _sample_rate = sample_rate;
    _is_receiver = is_receiver;
}

void RtcpContext::onRtp(uint16_t seq, uint32_t stamp, size_t bytes) {
    if (_is_receiver) {
        //接收者才做复杂的统计运算
        auto sys_stamp = getCurrentMillisecond();
        if (_last_rtp_sys_stamp) {
            //计算时间戳抖动值
            double diff = double(
                    int64_t(sys_stamp) - int64_t(_last_rtp_sys_stamp) - int64_t(stamp) + int64_t(_last_rtp_stamp));
            if (diff < 0) {
                diff = -diff;
            }
            //抖动单位为采样次数
            diff *= (_sample_rate / 1000.0);
            _jitter += (diff - _jitter) / 16.0;
        } else {
            _jitter = 0;
        }

        if (_last_rtp_seq > 0xFF00 && seq < 0xFF && (!_seq_cycles || _packets - _last_cycle_packets > 0x1FFF)) {
            //上次seq大于0xFF00且本次seq小于0xFF，
            //且未发生回环或者距离上次回环间隔超过0x1FFF个包，则认为回环
            ++_seq_cycles;
            _last_cycle_packets = _packets;
            _seq_max = seq;
        } else if (seq > _seq_max) {
            //本次回环前最大seq
            _seq_max = seq;
        }

        if (!_seq_base) {
            //记录第一个rtp的seq
            _seq_base = seq;
        } else if (!_seq_cycles && seq < _seq_base) {
            //未发生回环，那么取最新的seq为基准seq
            _seq_base = seq;
        }

        _last_rtp_seq = seq;
        _last_rtp_sys_stamp = sys_stamp;
    }

    ++_packets;
    _bytes += bytes;
    _last_rtp_stamp = stamp;
}

void RtcpContext::onRtcp(RtcpHeader *rtcp) {
    if ((RtcpType) rtcp->pt != RtcpType::RTCP_SR) {
        return;
    }
    if (!_is_receiver) {
        WarnL << "rtp发送者收到sr包";
        return;
    }
    auto rtcp_sr = (RtcpSR *) (rtcp);
    /**
     last SR timestamp (LSR): 32 bits
      The middle 32 bits out of 64 in the NTP timestamp (as explained in
      Section 4) received as part of the most recent RTCP sender report
      (SR) packet from source SSRC_n.  If no SR has been received yet,
      the field is set to zero.
     */
    _last_sr_lsr = ((rtcp_sr->ntpmsw & 0xFFFF) << 16) | ((rtcp_sr->ntplsw >> 16) & 0xFFFF);
    _last_sr_ntp_sys = getCurrentMillisecond();
}

size_t RtcpContext::getExpectedPackets() const {
    if (!_is_receiver) {
        throw std::runtime_error("rtp发送者无法统计应收包数");
    }
    return (_seq_cycles << 16) + _seq_max - _seq_base + 1;
}

size_t RtcpContext::getExpectedPacketsInterval() {
    auto expected = getExpectedPackets();
    auto ret = expected - _last_expected;
    _last_expected = expected;
    return ret;
}

size_t RtcpContext::getLost() {
    if (!_is_receiver) {
        throw std::runtime_error("rtp发送者无法统计丢包率");
    }
    return getExpectedPackets() - _packets;
}

size_t RtcpContext::geLostInterval() {
    auto lost = getLost();
    auto ret = lost - _last_lost;
    _last_lost = lost;
    return ret;
}

Buffer::Ptr RtcpContext::createRtcpSR(uint32_t rtcp_ssrc) {
    if (_is_receiver) {
        throw std::runtime_error("rtp接收者尝试发送sr包");
    }
    auto rtcp = RtcpSR::create(0);
    rtcp->ssrc = htonl(rtcp_ssrc);

    struct timeval tv;
    gettimeofday(&tv, NULL);
    rtcp->setNtpStamp(tv);

    //转换成rtp时间戳
    rtcp->rtpts = htonl(uint32_t(_last_rtp_stamp * (_sample_rate / 1000.0)));
    rtcp->packet_count = htonl((uint32_t) _packets);
    rtcp->octet_count = htonl((uint32_t) _bytes);
    return RtcpHeader::toBuffer(std::move(rtcp));
}

Buffer::Ptr RtcpContext::createRtcpRR(uint32_t rtcp_ssrc, uint32_t rtp_ssrc) {
    if (!_is_receiver) {
        throw std::runtime_error("rtp发送者尝试发送rr包");
    }
    auto rtcp = RtcpRR::create(1);
    rtcp->ssrc = htonl(rtcp_ssrc);

    ReportItem *item = (ReportItem *) &rtcp->items;
    item->ssrc = htonl(rtp_ssrc);

    uint8_t fraction = 0;
    auto expected_interval = getExpectedPacketsInterval();
    if (expected_interval) {
        fraction = uint8_t(geLostInterval() << 8 / expected_interval);
    }

    item->fraction = fraction;
    item->cumulative = htonl(uint32_t(getLost())) >> 8;
    item->seq_cycles = htons(_seq_cycles);
    item->seq_max = htons(_seq_max);
    item->jitter = htonl(uint32_t(_jitter));
    item->last_sr_stamp = htonl(_last_sr_lsr);

    // now - Last SR time,单位毫秒
    auto delay = getCurrentMillisecond() - _last_sr_ntp_sys;
    // in units of 1/65536 seconds
    auto dlsr = (uint32_t) (delay / 1000.0f * 65536);
    item->delay_since_last_sr = htonl(_last_sr_lsr ? dlsr : 0);
    return RtcpHeader::toBuffer(rtcp);
}

}//namespace mediakit