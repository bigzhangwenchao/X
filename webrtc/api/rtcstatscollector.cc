/*
 *  Copyright 2016 The WebRTC Project Authors. All rights reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include "webrtc/api/rtcstatscollector.h"

#include <memory>
#include <utility>
#include <vector>

#include "webrtc/api/peerconnection.h"
#include "webrtc/api/webrtcsession.h"
#include "webrtc/base/checks.h"
#include "webrtc/base/sslidentity.h"
#include "webrtc/p2p/base/candidate.h"
#include "webrtc/p2p/base/port.h"

namespace webrtc {

const char* CandidateTypeToRTCIceCandidateType(const std::string& type) {
  if (type == cricket::LOCAL_PORT_TYPE)
    return RTCIceCandidateType::kHost;
  if (type == cricket::STUN_PORT_TYPE)
    return RTCIceCandidateType::kSrflx;
  if (type == cricket::PRFLX_PORT_TYPE)
    return RTCIceCandidateType::kPrflx;
  if (type == cricket::RELAY_PORT_TYPE)
    return RTCIceCandidateType::kRelay;
  RTC_NOTREACHED();
  return nullptr;
}

rtc::scoped_refptr<RTCStatsCollector> RTCStatsCollector::Create(
    PeerConnection* pc, int64_t cache_lifetime_us) {
  return rtc::scoped_refptr<RTCStatsCollector>(
      new rtc::RefCountedObject<RTCStatsCollector>(pc, cache_lifetime_us));
}

RTCStatsCollector::RTCStatsCollector(PeerConnection* pc,
                                     int64_t cache_lifetime_us)
    : pc_(pc),
      signaling_thread_(pc->session()->signaling_thread()),
      worker_thread_(pc->session()->worker_thread()),
      network_thread_(pc->session()->network_thread()),
      num_pending_partial_reports_(0),
      partial_report_timestamp_us_(0),
      cache_timestamp_us_(0),
      cache_lifetime_us_(cache_lifetime_us) {
  RTC_DCHECK(pc_);
  RTC_DCHECK(signaling_thread_);
  RTC_DCHECK(worker_thread_);
  RTC_DCHECK(network_thread_);
  RTC_DCHECK_GE(cache_lifetime_us_, 0);
}

void RTCStatsCollector::GetStatsReport(
    rtc::scoped_refptr<RTCStatsCollectorCallback> callback) {
  RTC_DCHECK(signaling_thread_->IsCurrent());
  RTC_DCHECK(callback);
  callbacks_.push_back(callback);

  // "Now" using a monotonically increasing timer.
  int64_t cache_now_us = rtc::TimeMicros();
  if (cached_report_ &&
      cache_now_us - cache_timestamp_us_ <= cache_lifetime_us_) {
    // We have a fresh cached report to deliver.
    DeliverCachedReport();
  } else if (!num_pending_partial_reports_) {
    // Only start gathering stats if we're not already gathering stats. In the
    // case of already gathering stats, |callback_| will be invoked when there
    // are no more pending partial reports.

    // "Now" using a system clock, relative to the UNIX epoch (Jan 1, 1970,
    // UTC), in microseconds. The system clock could be modified and is not
    // necessarily monotonically increasing.
    int64_t timestamp_us = rtc::TimeUTCMicros();

    num_pending_partial_reports_ = 3;
    partial_report_timestamp_us_ = cache_now_us;
    invoker_.AsyncInvoke<void>(RTC_FROM_HERE, signaling_thread_,
        rtc::Bind(&RTCStatsCollector::ProducePartialResultsOnSignalingThread,
            rtc::scoped_refptr<RTCStatsCollector>(this), timestamp_us));
    invoker_.AsyncInvoke<void>(RTC_FROM_HERE, worker_thread_,
        rtc::Bind(&RTCStatsCollector::ProducePartialResultsOnWorkerThread,
            rtc::scoped_refptr<RTCStatsCollector>(this), timestamp_us));
    invoker_.AsyncInvoke<void>(RTC_FROM_HERE, network_thread_,
        rtc::Bind(&RTCStatsCollector::ProducePartialResultsOnNetworkThread,
            rtc::scoped_refptr<RTCStatsCollector>(this), timestamp_us));
  }
}

void RTCStatsCollector::ClearCachedStatsReport() {
  RTC_DCHECK(signaling_thread_->IsCurrent());
  cached_report_ = nullptr;
}

void RTCStatsCollector::ProducePartialResultsOnSignalingThread(
    int64_t timestamp_us) {
  RTC_DCHECK(signaling_thread_->IsCurrent());
  rtc::scoped_refptr<RTCStatsReport> report = RTCStatsReport::Create();

  SessionStats session_stats;
  if (pc_->session()->GetTransportStats(&session_stats)) {
    ProduceCertificateStats_s(timestamp_us, session_stats, report.get());
    ProduceIceCandidateAndPairStats_s(timestamp_us, session_stats,
                                      report.get());
  }
  ProducePeerConnectionStats_s(timestamp_us, report.get());

  AddPartialResults(report);
}

void RTCStatsCollector::ProducePartialResultsOnWorkerThread(
    int64_t timestamp_us) {
  RTC_DCHECK(worker_thread_->IsCurrent());
  rtc::scoped_refptr<RTCStatsReport> report = RTCStatsReport::Create();

  // TODO(hbos): Gather stats on worker thread.

  AddPartialResults(report);
}

void RTCStatsCollector::ProducePartialResultsOnNetworkThread(
    int64_t timestamp_us) {
  RTC_DCHECK(network_thread_->IsCurrent());
  rtc::scoped_refptr<RTCStatsReport> report = RTCStatsReport::Create();

  // TODO(hbos): Gather stats on network thread.

  AddPartialResults(report);
}

void RTCStatsCollector::AddPartialResults(
    const rtc::scoped_refptr<RTCStatsReport>& partial_report) {
  if (!signaling_thread_->IsCurrent()) {
    invoker_.AsyncInvoke<void>(RTC_FROM_HERE, signaling_thread_,
        rtc::Bind(&RTCStatsCollector::AddPartialResults_s,
                  rtc::scoped_refptr<RTCStatsCollector>(this),
                  partial_report));
    return;
  }
  AddPartialResults_s(partial_report);
}

void RTCStatsCollector::AddPartialResults_s(
    rtc::scoped_refptr<RTCStatsReport> partial_report) {
  RTC_DCHECK(signaling_thread_->IsCurrent());
  RTC_DCHECK_GT(num_pending_partial_reports_, 0);
  if (!partial_report_)
    partial_report_ = partial_report;
  else
    partial_report_->TakeMembersFrom(partial_report);
  --num_pending_partial_reports_;
  if (!num_pending_partial_reports_) {
    cache_timestamp_us_ = partial_report_timestamp_us_;
    cached_report_ = partial_report_;
    partial_report_ = nullptr;
    DeliverCachedReport();
  }
}

void RTCStatsCollector::DeliverCachedReport() {
  RTC_DCHECK(signaling_thread_->IsCurrent());
  RTC_DCHECK(!callbacks_.empty());
  RTC_DCHECK(cached_report_);
  for (const rtc::scoped_refptr<RTCStatsCollectorCallback>& callback :
       callbacks_) {
    callback->OnStatsDelivered(cached_report_);
  }
  callbacks_.clear();
}

void RTCStatsCollector::ProduceCertificateStats_s(
    int64_t timestamp_us, const SessionStats& session_stats,
    RTCStatsReport* report) const {
  RTC_DCHECK(signaling_thread_->IsCurrent());
  for (const auto& transport_stats : session_stats.transport_stats) {
    rtc::scoped_refptr<rtc::RTCCertificate> local_certificate;
    if (pc_->session()->GetLocalCertificate(
        transport_stats.second.transport_name, &local_certificate)) {
      ProduceCertificateStatsFromSSLCertificateAndChain_s(
          timestamp_us, local_certificate->ssl_certificate(), report);
    }
    std::unique_ptr<rtc::SSLCertificate> remote_certificate =
        pc_->session()->GetRemoteSSLCertificate(
            transport_stats.second.transport_name);
    if (remote_certificate) {
      ProduceCertificateStatsFromSSLCertificateAndChain_s(
          timestamp_us, *remote_certificate.get(), report);
    }
  }
}

void RTCStatsCollector::ProduceCertificateStatsFromSSLCertificateAndChain_s(
    int64_t timestamp_us, const rtc::SSLCertificate& certificate,
    RTCStatsReport* report) const {
  RTC_DCHECK(signaling_thread_->IsCurrent());
  std::unique_ptr<rtc::SSLCertificateStats> ssl_stats =
      certificate.GetStats();
  RTCCertificateStats* prev_stats = nullptr;
  for (rtc::SSLCertificateStats* s = ssl_stats.get(); s;
       s = s->issuer.get()) {
    RTCCertificateStats* stats = new RTCCertificateStats(
        "RTCCertificate_" + s->fingerprint, timestamp_us);
    stats->fingerprint = s->fingerprint;
    stats->fingerprint_algorithm = s->fingerprint_algorithm;
    stats->base64_certificate = s->base64_certificate;
    if (prev_stats)
      prev_stats->issuer_certificate_id = stats->id();
    report->AddStats(std::unique_ptr<RTCCertificateStats>(stats));
    prev_stats = stats;
  }
}

void RTCStatsCollector::ProduceIceCandidateAndPairStats_s(
      int64_t timestamp_us, const SessionStats& session_stats,
      RTCStatsReport* report) const {
  RTC_DCHECK(signaling_thread_->IsCurrent());
  for (const auto& transport_stats : session_stats.transport_stats) {
    for (const auto& channel_stats : transport_stats.second.channel_stats) {
      for (const cricket::ConnectionInfo& info :
           channel_stats.connection_infos) {
        const std::string& id = "RTCIceCandidatePair_" +
            info.local_candidate.id() + "_" + info.remote_candidate.id();
        std::unique_ptr<RTCIceCandidatePairStats> candidate_pair_stats(
            new RTCIceCandidatePairStats(id, timestamp_us));

        // TODO(hbos): Set all of the |RTCIceCandidatePairStats|'s members,
        // crbug.com/633550.

        // TODO(hbos): Set candidate_pair_stats->transport_id. Should be ID to
        // RTCTransportStats which does not exist yet: crbug.com/653873.

        // TODO(hbos): There could be other candidates that are not paired with
        // anything. We don't have a complete list. Local candidates come from
        // Port objects, and prflx candidates (both local and remote) are only
        // stored in candidate pairs. crbug.com/632723
        candidate_pair_stats->local_candidate_id = ProduceIceCandidateStats_s(
            timestamp_us, info.local_candidate, true, report);
        candidate_pair_stats->remote_candidate_id = ProduceIceCandidateStats_s(
            timestamp_us, info.remote_candidate, false, report);

        // TODO(hbos): Set candidate_pair_stats->state.
        // TODO(hbos): Set candidate_pair_stats->priority.
        // TODO(hbos): Set candidate_pair_stats->nominated.
        // TODO(hbos): This writable is different than the spec. It goes to
        // false after a certain amount of time without a response passes.
        // crbug.com/633550
        candidate_pair_stats->writable = info.writable;
        // TODO(hbos): Set candidate_pair_stats->readable.
        candidate_pair_stats->bytes_sent =
            static_cast<uint64_t>(info.sent_total_bytes);
        candidate_pair_stats->bytes_received =
            static_cast<uint64_t>(info.recv_total_bytes);
        // TODO(hbos): Set candidate_pair_stats->total_rtt.
        // TODO(hbos): The |info.rtt| measurement is smoothed. It shouldn't be
        // smoothed according to the spec. crbug.com/633550. See
        // https://w3c.github.io/webrtc-stats/#dom-rtcicecandidatepairstats-currentrtt
        candidate_pair_stats->current_rtt =
            static_cast<double>(info.rtt) / 1000.0;
        // TODO(hbos): Set candidate_pair_stats->available_outgoing_bitrate.
        // TODO(hbos): Set candidate_pair_stats->available_incoming_bitrate.
        // TODO(hbos): Set candidate_pair_stats->requests_received.
        candidate_pair_stats->requests_sent =
            static_cast<uint64_t>(info.sent_ping_requests_total);
        candidate_pair_stats->responses_received =
            static_cast<uint64_t>(info.recv_ping_responses);
        candidate_pair_stats->responses_sent =
            static_cast<uint64_t>(info.sent_ping_responses);
        // TODO(hbos): Set candidate_pair_stats->retransmissions_received.
        // TODO(hbos): Set candidate_pair_stats->retransmissions_sent.
        // TODO(hbos): Set candidate_pair_stats->consent_requests_received.
        // TODO(hbos): Set candidate_pair_stats->consent_requests_sent.
        // TODO(hbos): Set candidate_pair_stats->consent_responses_received.
        // TODO(hbos): Set candidate_pair_stats->consent_responses_sent.

        report->AddStats(std::move(candidate_pair_stats));
      }
    }
  }
}

const std::string& RTCStatsCollector::ProduceIceCandidateStats_s(
    int64_t timestamp_us, const cricket::Candidate& candidate, bool is_local,
    RTCStatsReport* report) const {
  RTC_DCHECK(signaling_thread_->IsCurrent());
  const std::string& id = "RTCIceCandidate_" + candidate.id();
  const RTCStats* stats = report->Get(id);
  if (!stats) {
    std::unique_ptr<RTCIceCandidateStats> candidate_stats;
    if (is_local)
      candidate_stats.reset(new RTCLocalIceCandidateStats(id, timestamp_us));
    else
      candidate_stats.reset(new RTCRemoteIceCandidateStats(id, timestamp_us));
    candidate_stats->ip = candidate.address().ipaddr().ToString();
    candidate_stats->port = static_cast<int32_t>(candidate.address().port());
    candidate_stats->protocol = candidate.protocol();
    candidate_stats->candidate_type = CandidateTypeToRTCIceCandidateType(
        candidate.type());
    candidate_stats->priority = static_cast<int32_t>(candidate.priority());
    // TODO(hbos): Define candidate_stats->url. crbug.com/632723

    stats = candidate_stats.get();
    report->AddStats(std::move(candidate_stats));
  }
  RTC_DCHECK_EQ(stats->type(), is_local ? RTCLocalIceCandidateStats::kType
                                        : RTCRemoteIceCandidateStats::kType);
  return stats->id();
}

void RTCStatsCollector::ProducePeerConnectionStats_s(
    int64_t timestamp_us, RTCStatsReport* report) const {
  RTC_DCHECK(signaling_thread_->IsCurrent());
  // TODO(hbos): If data channels are removed from the peer connection this will
  // yield incorrect counts. Address before closing crbug.com/636818. See
  // https://w3c.github.io/webrtc-stats/webrtc-stats.html#pcstats-dict*.
  uint32_t data_channels_opened = 0;
  const std::vector<rtc::scoped_refptr<DataChannel>>& data_channels =
      pc_->sctp_data_channels();
  for (const rtc::scoped_refptr<DataChannel>& data_channel : data_channels) {
    if (data_channel->state() == DataChannelInterface::kOpen)
      ++data_channels_opened;
  }
  // There is always just one |RTCPeerConnectionStats| so its |id| can be a
  // constant.
  std::unique_ptr<RTCPeerConnectionStats> stats(
    new RTCPeerConnectionStats("RTCPeerConnection", timestamp_us));
  stats->data_channels_opened = data_channels_opened;
  stats->data_channels_closed = static_cast<uint32_t>(data_channels.size()) -
                                data_channels_opened;
  report->AddStats(std::move(stats));
}

}  // namespace webrtc
