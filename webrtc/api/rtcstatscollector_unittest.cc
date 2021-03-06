/*
 *  Copyright 2016 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include "webrtc/api/rtcstatscollector.h"

#include <memory>
#include <string>
#include <vector>

#include "webrtc/api/jsepsessiondescription.h"
#include "webrtc/api/stats/rtcstats_objects.h"
#include "webrtc/api/stats/rtcstatsreport.h"
#include "webrtc/api/test/mock_datachannel.h"
#include "webrtc/api/test/mock_peerconnection.h"
#include "webrtc/api/test/mock_webrtcsession.h"
#include "webrtc/base/checks.h"
#include "webrtc/base/fakeclock.h"
#include "webrtc/base/fakesslidentity.h"
#include "webrtc/base/gunit.h"
#include "webrtc/base/logging.h"
#include "webrtc/base/socketaddress.h"
#include "webrtc/base/thread_checker.h"
#include "webrtc/base/timedelta.h"
#include "webrtc/base/timeutils.h"
#include "webrtc/logging/rtc_event_log/rtc_event_log.h"
#include "webrtc/media/base/fakemediaengine.h"
#include "webrtc/p2p/base/port.h"

using testing::_;
using testing::Invoke;
using testing::Return;
using testing::ReturnRef;

namespace webrtc {

namespace {

const int64_t kGetStatsReportTimeoutMs = 1000;

struct CertificateInfo {
  rtc::scoped_refptr<rtc::RTCCertificate> certificate;
  std::vector<std::string> ders;
  std::vector<std::string> pems;
  std::vector<std::string> fingerprints;
};

std::unique_ptr<CertificateInfo> CreateFakeCertificateAndInfoFromDers(
    const std::vector<std::string>& ders) {
  RTC_CHECK(!ders.empty());
  std::unique_ptr<CertificateInfo> info(new CertificateInfo());
  info->ders = ders;
  for (const std::string& der : ders) {
    info->pems.push_back(rtc::SSLIdentity::DerToPem(
        "CERTIFICATE",
        reinterpret_cast<const unsigned char*>(der.c_str()),
        der.length()));
  }
  info->certificate =
      rtc::RTCCertificate::Create(std::unique_ptr<rtc::FakeSSLIdentity>(
          new rtc::FakeSSLIdentity(rtc::FakeSSLCertificate(info->pems))));
  // Strip header/footer and newline characters of PEM strings.
  for (size_t i = 0; i < info->pems.size(); ++i) {
    rtc::replace_substrs("-----BEGIN CERTIFICATE-----", 27,
                         "", 0, &info->pems[i]);
    rtc::replace_substrs("-----END CERTIFICATE-----", 25,
                         "", 0, &info->pems[i]);
    rtc::replace_substrs("\n", 1,
                         "", 0, &info->pems[i]);
  }
  // Fingerprint of leaf certificate.
  std::unique_ptr<rtc::SSLFingerprint> fp(
      rtc::SSLFingerprint::Create("sha-1",
                                  &info->certificate->ssl_certificate()));
  EXPECT_TRUE(fp);
  info->fingerprints.push_back(fp->GetRfc4572Fingerprint());
  // Fingerprints of the rest of the chain.
  std::unique_ptr<rtc::SSLCertChain> chain =
      info->certificate->ssl_certificate().GetChain();
  if (chain) {
    for (size_t i = 0; i < chain->GetSize(); i++) {
      fp.reset(rtc::SSLFingerprint::Create("sha-1", &chain->Get(i)));
      EXPECT_TRUE(fp);
      info->fingerprints.push_back(fp->GetRfc4572Fingerprint());
    }
  }
  EXPECT_EQ(info->ders.size(), info->fingerprints.size());
  return info;
}

std::unique_ptr<cricket::Candidate> CreateFakeCandidate(
    const std::string& hostname,
    int port,
    const std::string& protocol,
    const std::string& candidate_type,
    uint32_t priority) {
  std::unique_ptr<cricket::Candidate> candidate(new cricket::Candidate());
  candidate->set_address(rtc::SocketAddress(hostname, port));
  candidate->set_protocol(protocol);
  candidate->set_type(candidate_type);
  candidate->set_priority(priority);
  return candidate;
}

class RTCStatsCollectorTestHelper : public SetSessionDescriptionObserver {
 public:
  RTCStatsCollectorTestHelper()
      : worker_thread_(rtc::Thread::Current()),
        network_thread_(rtc::Thread::Current()),
        channel_manager_(
            new cricket::ChannelManager(new cricket::FakeMediaEngine(),
                                        worker_thread_,
                                        network_thread_)),
        media_controller_(
            MediaControllerInterface::Create(cricket::MediaConfig(),
                                             worker_thread_,
                                             channel_manager_.get(),
                                             &event_log_)),
        session_(media_controller_.get()),
        pc_() {
    // Default return values for mocks.
    EXPECT_CALL(pc_, session()).WillRepeatedly(Return(&session_));
    EXPECT_CALL(pc_, sctp_data_channels()).WillRepeatedly(
        ReturnRef(data_channels_));
    EXPECT_CALL(session_, GetTransportStats(_)).WillRepeatedly(Return(false));
    EXPECT_CALL(session_, GetLocalCertificate(_, _)).WillRepeatedly(
        Return(false));
    EXPECT_CALL(session_, GetRemoteSSLCertificate_ReturnsRawPointer(_))
        .WillRepeatedly(Return(nullptr));
  }

  rtc::ScopedFakeClock& fake_clock() { return fake_clock_; }
  MockWebRtcSession& session() { return session_; }
  MockPeerConnection& pc() { return pc_; }
  std::vector<rtc::scoped_refptr<DataChannel>>& data_channels() {
    return data_channels_;
  }

  // SetSessionDescriptionObserver overrides.
  void OnSuccess() override {}
  void OnFailure(const std::string& error) override {
    RTC_NOTREACHED() << error;
  }

 private:
  rtc::ScopedFakeClock fake_clock_;
  webrtc::RtcEventLogNullImpl event_log_;
  rtc::Thread* const worker_thread_;
  rtc::Thread* const network_thread_;
  std::unique_ptr<cricket::ChannelManager> channel_manager_;
  std::unique_ptr<webrtc::MediaControllerInterface> media_controller_;
  MockWebRtcSession session_;
  MockPeerConnection pc_;

  std::vector<rtc::scoped_refptr<DataChannel>> data_channels_;
};

class RTCTestStats : public RTCStats {
 public:
  WEBRTC_RTCSTATS_DECL();

  RTCTestStats(const std::string& id, int64_t timestamp_us)
      : RTCStats(id, timestamp_us),
        dummy_stat("dummyStat") {}

  RTCStatsMember<int32_t> dummy_stat;
};

WEBRTC_RTCSTATS_IMPL(RTCTestStats, RTCStats, "test-stats",
    &dummy_stat);

// Overrides the stats collection to verify thread usage and that the resulting
// partial reports are merged.
class FakeRTCStatsCollector : public RTCStatsCollector,
                              public RTCStatsCollectorCallback {
 public:
  static rtc::scoped_refptr<FakeRTCStatsCollector> Create(
      PeerConnection* pc,
      int64_t cache_lifetime_us) {
    return rtc::scoped_refptr<FakeRTCStatsCollector>(
        new rtc::RefCountedObject<FakeRTCStatsCollector>(
            pc, cache_lifetime_us));
  }

  // RTCStatsCollectorCallback implementation.
  void OnStatsDelivered(
      const rtc::scoped_refptr<const RTCStatsReport>& report) override {
    EXPECT_TRUE(signaling_thread_->IsCurrent());
    rtc::CritScope cs(&lock_);
    delivered_report_ = report;
  }

  void VerifyThreadUsageAndResultsMerging() {
    GetStatsReport(rtc::scoped_refptr<RTCStatsCollectorCallback>(this));
    EXPECT_TRUE_WAIT(HasVerifiedResults(), kGetStatsReportTimeoutMs);
  }

  bool HasVerifiedResults() {
    EXPECT_TRUE(signaling_thread_->IsCurrent());
    rtc::CritScope cs(&lock_);
    if (!delivered_report_)
      return false;
    EXPECT_EQ(produced_on_signaling_thread_, 1);
    EXPECT_EQ(produced_on_worker_thread_, 1);
    EXPECT_EQ(produced_on_network_thread_, 1);

    EXPECT_TRUE(delivered_report_->Get("SignalingThreadStats"));
    EXPECT_TRUE(delivered_report_->Get("WorkerThreadStats"));
    EXPECT_TRUE(delivered_report_->Get("NetworkThreadStats"));

    produced_on_signaling_thread_ = 0;
    produced_on_worker_thread_ = 0;
    produced_on_network_thread_ = 0;
    delivered_report_ = nullptr;
    return true;
  }

 protected:
  FakeRTCStatsCollector(
      PeerConnection* pc,
      int64_t cache_lifetime)
      : RTCStatsCollector(pc, cache_lifetime),
        signaling_thread_(pc->session()->signaling_thread()),
        worker_thread_(pc->session()->worker_thread()),
        network_thread_(pc->session()->network_thread()) {
  }

  void ProducePartialResultsOnSignalingThread(int64_t timestamp_us) override {
    EXPECT_TRUE(signaling_thread_->IsCurrent());
    {
      rtc::CritScope cs(&lock_);
      EXPECT_FALSE(delivered_report_);
      ++produced_on_signaling_thread_;
    }

    rtc::scoped_refptr<RTCStatsReport> signaling_report =
        RTCStatsReport::Create();
    signaling_report->AddStats(std::unique_ptr<const RTCStats>(
        new RTCTestStats("SignalingThreadStats", timestamp_us)));
    AddPartialResults(signaling_report);
  }
  void ProducePartialResultsOnWorkerThread(int64_t timestamp_us) override {
    EXPECT_TRUE(worker_thread_->IsCurrent());
    {
      rtc::CritScope cs(&lock_);
      EXPECT_FALSE(delivered_report_);
      ++produced_on_worker_thread_;
    }

    rtc::scoped_refptr<RTCStatsReport> worker_report = RTCStatsReport::Create();
    worker_report->AddStats(std::unique_ptr<const RTCStats>(
        new RTCTestStats("WorkerThreadStats", timestamp_us)));
    AddPartialResults(worker_report);
  }
  void ProducePartialResultsOnNetworkThread(int64_t timestamp_us) override {
    EXPECT_TRUE(network_thread_->IsCurrent());
    {
      rtc::CritScope cs(&lock_);
      EXPECT_FALSE(delivered_report_);
      ++produced_on_network_thread_;
    }

    rtc::scoped_refptr<RTCStatsReport> network_report =
        RTCStatsReport::Create();
    network_report->AddStats(std::unique_ptr<const RTCStats>(
        new RTCTestStats("NetworkThreadStats", timestamp_us)));
    AddPartialResults(network_report);
  }

 private:
  rtc::Thread* const signaling_thread_;
  rtc::Thread* const worker_thread_;
  rtc::Thread* const network_thread_;

  rtc::CriticalSection lock_;
  rtc::scoped_refptr<const RTCStatsReport> delivered_report_;
  int produced_on_signaling_thread_ = 0;
  int produced_on_worker_thread_ = 0;
  int produced_on_network_thread_ = 0;
};

class StatsCallback : public RTCStatsCollectorCallback {
 public:
  static rtc::scoped_refptr<StatsCallback> Create(
      rtc::scoped_refptr<const RTCStatsReport>* report_ptr = nullptr) {
    return rtc::scoped_refptr<StatsCallback>(
        new rtc::RefCountedObject<StatsCallback>(report_ptr));
  }

  void OnStatsDelivered(
      const rtc::scoped_refptr<const RTCStatsReport>& report) override {
    EXPECT_TRUE(thread_checker_.CalledOnValidThread());
    report_ = report;
    if (report_ptr_)
      *report_ptr_ = report_;
  }

  rtc::scoped_refptr<const RTCStatsReport> report() const {
    EXPECT_TRUE(thread_checker_.CalledOnValidThread());
    return report_;
  }

 protected:
  explicit StatsCallback(rtc::scoped_refptr<const RTCStatsReport>* report_ptr)
      : report_ptr_(report_ptr) {}

 private:
  rtc::ThreadChecker thread_checker_;
  rtc::scoped_refptr<const RTCStatsReport> report_;
  rtc::scoped_refptr<const RTCStatsReport>* report_ptr_;
};

class RTCStatsCollectorTest : public testing::Test {
 public:
  RTCStatsCollectorTest()
    : test_(new rtc::RefCountedObject<RTCStatsCollectorTestHelper>()),
      collector_(RTCStatsCollector::Create(
          &test_->pc(), 50 * rtc::kNumMicrosecsPerMillisec)) {
  }

  rtc::scoped_refptr<const RTCStatsReport> GetStatsReport() {
    rtc::scoped_refptr<StatsCallback> callback = StatsCallback::Create();
    collector_->GetStatsReport(callback);
    EXPECT_TRUE_WAIT(callback->report(), kGetStatsReportTimeoutMs);
    return callback->report();
  }

  const RTCIceCandidateStats* ExpectReportContainsCandidate(
      const rtc::scoped_refptr<const RTCStatsReport>& report,
      const cricket::Candidate& candidate,
      bool is_local) {
    const RTCStats* stats = report->Get("RTCIceCandidate_" + candidate.id());
    EXPECT_TRUE(stats);
    const RTCIceCandidateStats* candidate_stats;
    if (is_local)
        candidate_stats = &stats->cast_to<RTCLocalIceCandidateStats>();
    else
        candidate_stats = &stats->cast_to<RTCRemoteIceCandidateStats>();
    EXPECT_EQ(*candidate_stats->ip, candidate.address().ipaddr().ToString());
    EXPECT_EQ(*candidate_stats->port,
              static_cast<int32_t>(candidate.address().port()));
    EXPECT_EQ(*candidate_stats->protocol, candidate.protocol());
    EXPECT_EQ(*candidate_stats->candidate_type,
              CandidateTypeToRTCIceCandidateType(candidate.type()));
    EXPECT_EQ(*candidate_stats->priority,
              static_cast<int32_t>(candidate.priority()));
    // TODO(hbos): Define candidate_stats->url. crbug.com/632723
    EXPECT_FALSE(candidate_stats->url.is_defined());
    return candidate_stats;
  }

  void ExpectReportContainsCandidatePair(
      const rtc::scoped_refptr<const RTCStatsReport>& report,
      const cricket::TransportStats& transport_stats) {
    for (const auto& channel_stats : transport_stats.channel_stats) {
      for (const cricket::ConnectionInfo& info :
           channel_stats.connection_infos) {
        const std::string& id = "RTCIceCandidatePair_" +
            info.local_candidate.id() + "_" + info.remote_candidate.id();
        const RTCStats* stats = report->Get(id);
        EXPECT_TRUE(stats);
        const RTCIceCandidatePairStats& candidate_pair_stats =
            stats->cast_to<RTCIceCandidatePairStats>();

        // TODO(hbos): Define all the undefined |candidate_pair_stats| stats.
        // The EXPECT_FALSE are for the undefined stats, see also todos listed
        // in rtcstatscollector.cc. crbug.com/633550
        EXPECT_FALSE(candidate_pair_stats.transport_id.is_defined());
        const RTCIceCandidateStats* local_candidate =
            ExpectReportContainsCandidate(report, info.local_candidate, true);
        EXPECT_EQ(*candidate_pair_stats.local_candidate_id,
                  local_candidate->id());
        const RTCIceCandidateStats* remote_candidate =
            ExpectReportContainsCandidate(report, info.remote_candidate, false);
        EXPECT_EQ(*candidate_pair_stats.remote_candidate_id,
                  remote_candidate->id());

        EXPECT_FALSE(candidate_pair_stats.state.is_defined());
        EXPECT_FALSE(candidate_pair_stats.priority.is_defined());
        EXPECT_FALSE(candidate_pair_stats.nominated.is_defined());
        EXPECT_EQ(*candidate_pair_stats.writable, info.writable);
        EXPECT_FALSE(candidate_pair_stats.readable.is_defined());
        EXPECT_EQ(*candidate_pair_stats.bytes_sent,
                  static_cast<uint64_t>(info.sent_total_bytes));
        EXPECT_EQ(*candidate_pair_stats.bytes_received,
                  static_cast<uint64_t>(info.recv_total_bytes));
        EXPECT_FALSE(candidate_pair_stats.total_rtt.is_defined());
        EXPECT_EQ(*candidate_pair_stats.current_rtt,
                  static_cast<double>(info.rtt) / 1000.0);
        EXPECT_FALSE(
            candidate_pair_stats.available_outgoing_bitrate.is_defined());
        EXPECT_FALSE(
            candidate_pair_stats.available_incoming_bitrate.is_defined());
        EXPECT_FALSE(candidate_pair_stats.requests_received.is_defined());
        EXPECT_EQ(*candidate_pair_stats.requests_sent,
                  static_cast<uint64_t>(info.sent_ping_requests_total));
        EXPECT_EQ(*candidate_pair_stats.responses_received,
                  static_cast<uint64_t>(info.recv_ping_responses));
        EXPECT_EQ(*candidate_pair_stats.responses_sent,
                  static_cast<uint64_t>(info.sent_ping_responses));
        EXPECT_FALSE(
            candidate_pair_stats.retransmissions_received.is_defined());
        EXPECT_FALSE(candidate_pair_stats.retransmissions_sent.is_defined());
        EXPECT_FALSE(
            candidate_pair_stats.consent_requests_received.is_defined());
        EXPECT_FALSE(candidate_pair_stats.consent_requests_sent.is_defined());
        EXPECT_FALSE(
            candidate_pair_stats.consent_responses_received.is_defined());
        EXPECT_FALSE(candidate_pair_stats.consent_responses_sent.is_defined());
      }
    }
  }

  void ExpectReportContainsCertificateInfo(
      const rtc::scoped_refptr<const RTCStatsReport>& report,
      const CertificateInfo& cert_info) {
    for (size_t i = 0; i < cert_info.fingerprints.size(); ++i) {
      const RTCStats* stats = report->Get(
          "RTCCertificate_" + cert_info.fingerprints[i]);
      EXPECT_TRUE(stats);
      const RTCCertificateStats& cert_stats =
          stats->cast_to<const RTCCertificateStats>();
      EXPECT_EQ(*cert_stats.fingerprint, cert_info.fingerprints[i]);
      EXPECT_EQ(*cert_stats.fingerprint_algorithm, "sha-1");
      EXPECT_EQ(*cert_stats.base64_certificate, cert_info.pems[i]);
      if (i + 1 < cert_info.fingerprints.size()) {
        EXPECT_EQ(*cert_stats.issuer_certificate_id,
                  "RTCCertificate_" + cert_info.fingerprints[i + 1]);
      } else {
        EXPECT_FALSE(cert_stats.issuer_certificate_id.is_defined());
      }
    }
  }

 protected:
  rtc::scoped_refptr<RTCStatsCollectorTestHelper> test_;
  rtc::scoped_refptr<RTCStatsCollector> collector_;
};

TEST_F(RTCStatsCollectorTest, SingleCallback) {
  rtc::scoped_refptr<const RTCStatsReport> result;
  collector_->GetStatsReport(StatsCallback::Create(&result));
  EXPECT_TRUE_WAIT(result, kGetStatsReportTimeoutMs);
}

TEST_F(RTCStatsCollectorTest, MultipleCallbacks) {
  rtc::scoped_refptr<const RTCStatsReport> a;
  rtc::scoped_refptr<const RTCStatsReport> b;
  rtc::scoped_refptr<const RTCStatsReport> c;
  collector_->GetStatsReport(StatsCallback::Create(&a));
  collector_->GetStatsReport(StatsCallback::Create(&b));
  collector_->GetStatsReport(StatsCallback::Create(&c));
  EXPECT_TRUE_WAIT(a, kGetStatsReportTimeoutMs);
  EXPECT_TRUE_WAIT(b, kGetStatsReportTimeoutMs);
  EXPECT_TRUE_WAIT(c, kGetStatsReportTimeoutMs);
  EXPECT_EQ(a.get(), b.get());
  EXPECT_EQ(b.get(), c.get());
}

TEST_F(RTCStatsCollectorTest, CachedStatsReports) {
  // Caching should ensure |a| and |b| are the same report.
  rtc::scoped_refptr<const RTCStatsReport> a = GetStatsReport();
  rtc::scoped_refptr<const RTCStatsReport> b = GetStatsReport();
  EXPECT_EQ(a.get(), b.get());
  // Invalidate cache by clearing it.
  collector_->ClearCachedStatsReport();
  rtc::scoped_refptr<const RTCStatsReport> c = GetStatsReport();
  EXPECT_NE(b.get(), c.get());
  // Invalidate cache by advancing time.
  test_->fake_clock().AdvanceTime(rtc::TimeDelta::FromMilliseconds(51));
  rtc::scoped_refptr<const RTCStatsReport> d = GetStatsReport();
  EXPECT_TRUE(d);
  EXPECT_NE(c.get(), d.get());
}

TEST_F(RTCStatsCollectorTest, MultipleCallbacksWithInvalidatedCacheInBetween) {
  rtc::scoped_refptr<const RTCStatsReport> a;
  rtc::scoped_refptr<const RTCStatsReport> b;
  rtc::scoped_refptr<const RTCStatsReport> c;
  collector_->GetStatsReport(StatsCallback::Create(&a));
  collector_->GetStatsReport(StatsCallback::Create(&b));
  // Cache is invalidated after 50 ms.
  test_->fake_clock().AdvanceTime(rtc::TimeDelta::FromMilliseconds(51));
  collector_->GetStatsReport(StatsCallback::Create(&c));
  EXPECT_TRUE_WAIT(a, kGetStatsReportTimeoutMs);
  EXPECT_TRUE_WAIT(b, kGetStatsReportTimeoutMs);
  EXPECT_TRUE_WAIT(c, kGetStatsReportTimeoutMs);
  EXPECT_EQ(a.get(), b.get());
  // The act of doing |AdvanceTime| processes all messages. If this was not the
  // case we might not require |c| to be fresher than |b|.
  EXPECT_NE(c.get(), b.get());
}

TEST_F(RTCStatsCollectorTest, CollectRTCCertificateStatsSingle) {
  std::unique_ptr<CertificateInfo> local_certinfo =
      CreateFakeCertificateAndInfoFromDers(
          std::vector<std::string>({ "(local) single certificate" }));
  std::unique_ptr<CertificateInfo> remote_certinfo =
      CreateFakeCertificateAndInfoFromDers(
          std::vector<std::string>({ "(remote) single certificate" }));

  // Mock the session to return the local and remote certificates.
  EXPECT_CALL(test_->session(), GetTransportStats(_)).WillRepeatedly(Invoke(
      [this](SessionStats* stats) {
        stats->transport_stats["transport"].transport_name = "transport";
        return true;
      }));
  EXPECT_CALL(test_->session(), GetLocalCertificate(_, _)).WillRepeatedly(
      Invoke([this, &local_certinfo](const std::string& transport_name,
             rtc::scoped_refptr<rtc::RTCCertificate>* certificate) {
        if (transport_name == "transport") {
          *certificate = local_certinfo->certificate;
          return true;
        }
        return false;
      }));
  EXPECT_CALL(test_->session(),
      GetRemoteSSLCertificate_ReturnsRawPointer(_)).WillRepeatedly(Invoke(
      [this, &remote_certinfo](const std::string& transport_name) {
        if (transport_name == "transport")
          return remote_certinfo->certificate->ssl_certificate().GetReference();
        return static_cast<rtc::SSLCertificate*>(nullptr);
      }));

  rtc::scoped_refptr<const RTCStatsReport> report = GetStatsReport();
  ExpectReportContainsCertificateInfo(report, *local_certinfo.get());
  ExpectReportContainsCertificateInfo(report, *remote_certinfo.get());
}

TEST_F(RTCStatsCollectorTest, CollectRTCCertificateStatsMultiple) {
  std::unique_ptr<CertificateInfo> audio_local_certinfo =
      CreateFakeCertificateAndInfoFromDers(
          std::vector<std::string>({ "(local) audio" }));
  audio_local_certinfo = CreateFakeCertificateAndInfoFromDers(
      audio_local_certinfo->ders);
  std::unique_ptr<CertificateInfo> audio_remote_certinfo =
      CreateFakeCertificateAndInfoFromDers(
          std::vector<std::string>({ "(remote) audio" }));
  audio_remote_certinfo = CreateFakeCertificateAndInfoFromDers(
      audio_remote_certinfo->ders);

  std::unique_ptr<CertificateInfo> video_local_certinfo =
      CreateFakeCertificateAndInfoFromDers(
          std::vector<std::string>({ "(local) video" }));
  video_local_certinfo = CreateFakeCertificateAndInfoFromDers(
      video_local_certinfo->ders);
  std::unique_ptr<CertificateInfo> video_remote_certinfo =
      CreateFakeCertificateAndInfoFromDers(
          std::vector<std::string>({ "(remote) video" }));
  video_remote_certinfo = CreateFakeCertificateAndInfoFromDers(
      video_remote_certinfo->ders);

  // Mock the session to return the local and remote certificates.
  EXPECT_CALL(test_->session(), GetTransportStats(_)).WillRepeatedly(Invoke(
      [this](SessionStats* stats) {
        stats->transport_stats["audio"].transport_name = "audio";
        stats->transport_stats["video"].transport_name = "video";
        return true;
      }));
  EXPECT_CALL(test_->session(), GetLocalCertificate(_, _)).WillRepeatedly(
      Invoke([this, &audio_local_certinfo, &video_local_certinfo](
            const std::string& transport_name,
            rtc::scoped_refptr<rtc::RTCCertificate>* certificate) {
        if (transport_name == "audio") {
          *certificate = audio_local_certinfo->certificate;
          return true;
        }
        if (transport_name == "video") {
          *certificate = video_local_certinfo->certificate;
          return true;
        }
        return false;
      }));
  EXPECT_CALL(test_->session(),
      GetRemoteSSLCertificate_ReturnsRawPointer(_)).WillRepeatedly(Invoke(
      [this, &audio_remote_certinfo, &video_remote_certinfo](
          const std::string& transport_name) {
        if (transport_name == "audio") {
          return audio_remote_certinfo->certificate->ssl_certificate()
              .GetReference();
        }
        if (transport_name == "video") {
          return video_remote_certinfo->certificate->ssl_certificate()
              .GetReference();
        }
        return static_cast<rtc::SSLCertificate*>(nullptr);
      }));

  rtc::scoped_refptr<const RTCStatsReport> report = GetStatsReport();
  ExpectReportContainsCertificateInfo(report, *audio_local_certinfo.get());
  ExpectReportContainsCertificateInfo(report, *audio_remote_certinfo.get());
  ExpectReportContainsCertificateInfo(report, *video_local_certinfo.get());
  ExpectReportContainsCertificateInfo(report, *video_remote_certinfo.get());
}

TEST_F(RTCStatsCollectorTest, CollectRTCCertificateStatsChain) {
  std::vector<std::string> local_ders;
  local_ders.push_back("(local) this");
  local_ders.push_back("(local) is");
  local_ders.push_back("(local) a");
  local_ders.push_back("(local) chain");
  std::unique_ptr<CertificateInfo> local_certinfo =
      CreateFakeCertificateAndInfoFromDers(local_ders);
  std::vector<std::string> remote_ders;
  remote_ders.push_back("(remote) this");
  remote_ders.push_back("(remote) is");
  remote_ders.push_back("(remote) another");
  remote_ders.push_back("(remote) chain");
  std::unique_ptr<CertificateInfo> remote_certinfo =
      CreateFakeCertificateAndInfoFromDers(remote_ders);

  // Mock the session to return the local and remote certificates.
  EXPECT_CALL(test_->session(), GetTransportStats(_)).WillRepeatedly(Invoke(
      [this](SessionStats* stats) {
        stats->transport_stats["transport"].transport_name = "transport";
        return true;
      }));
  EXPECT_CALL(test_->session(), GetLocalCertificate(_, _)).WillRepeatedly(
      Invoke([this, &local_certinfo](const std::string& transport_name,
             rtc::scoped_refptr<rtc::RTCCertificate>* certificate) {
        if (transport_name == "transport") {
          *certificate = local_certinfo->certificate;
          return true;
        }
        return false;
      }));
  EXPECT_CALL(test_->session(),
      GetRemoteSSLCertificate_ReturnsRawPointer(_)).WillRepeatedly(Invoke(
      [this, &remote_certinfo](const std::string& transport_name) {
        if (transport_name == "transport")
          return remote_certinfo->certificate->ssl_certificate().GetReference();
        return static_cast<rtc::SSLCertificate*>(nullptr);
      }));

  rtc::scoped_refptr<const RTCStatsReport> report = GetStatsReport();
  ExpectReportContainsCertificateInfo(report, *local_certinfo.get());
  ExpectReportContainsCertificateInfo(report, *remote_certinfo.get());
}

TEST_F(RTCStatsCollectorTest, CollectRTCIceCandidateStats) {
  // Candidates in the first transport stats.
  std::unique_ptr<cricket::Candidate> a_local_host = CreateFakeCandidate(
      "1.2.3.4", 5,
      "a_local_host's protocol",
      cricket::LOCAL_PORT_TYPE,
      0);
  std::unique_ptr<cricket::Candidate> a_remote_srflx = CreateFakeCandidate(
      "6.7.8.9", 10,
      "remote_srflx's protocol",
      cricket::STUN_PORT_TYPE,
      1);
  std::unique_ptr<cricket::Candidate> a_local_prflx = CreateFakeCandidate(
      "11.12.13.14", 15,
      "a_local_prflx's protocol",
      cricket::PRFLX_PORT_TYPE,
      2);
  std::unique_ptr<cricket::Candidate> a_remote_relay = CreateFakeCandidate(
      "16.17.18.19", 20,
      "a_remote_relay's protocol",
      cricket::RELAY_PORT_TYPE,
      3);
  // Candidates in the second transport stats.
  std::unique_ptr<cricket::Candidate> b_local = CreateFakeCandidate(
      "42.42.42.42", 42,
      "b_local's protocol",
      cricket::LOCAL_PORT_TYPE,
      42);
  std::unique_ptr<cricket::Candidate> b_remote = CreateFakeCandidate(
      "42.42.42.42", 42,
      "b_remote's protocol",
      cricket::LOCAL_PORT_TYPE,
      42);

  SessionStats session_stats;

  cricket::TransportChannelStats a_transport_channel_stats;
  a_transport_channel_stats.connection_infos.push_back(
      cricket::ConnectionInfo());
  a_transport_channel_stats.connection_infos[0].local_candidate =
      *a_local_host.get();
  a_transport_channel_stats.connection_infos[0].remote_candidate =
      *a_remote_srflx.get();
  a_transport_channel_stats.connection_infos.push_back(
      cricket::ConnectionInfo());
  a_transport_channel_stats.connection_infos[1].local_candidate =
      *a_local_prflx.get();
  a_transport_channel_stats.connection_infos[1].remote_candidate =
      *a_remote_relay.get();
  session_stats.transport_stats["a"].channel_stats.push_back(
      a_transport_channel_stats);

  cricket::TransportChannelStats b_transport_channel_stats;
  b_transport_channel_stats.connection_infos.push_back(
      cricket::ConnectionInfo());
  b_transport_channel_stats.connection_infos[0].local_candidate =
      *b_local.get();
  b_transport_channel_stats.connection_infos[0].remote_candidate =
      *b_remote.get();
  session_stats.transport_stats["b"].channel_stats.push_back(
      b_transport_channel_stats);

  // Mock the session to return the desired candidates.
  EXPECT_CALL(test_->session(), GetTransportStats(_)).WillRepeatedly(Invoke(
      [this, &session_stats](SessionStats* stats) {
        *stats = session_stats;
        return true;
      }));

  rtc::scoped_refptr<const RTCStatsReport> report = GetStatsReport();
  ExpectReportContainsCandidate(report, *a_local_host.get(), true);
  ExpectReportContainsCandidate(report, *a_remote_srflx.get(), false);
  ExpectReportContainsCandidate(report, *a_local_prflx.get(), true);
  ExpectReportContainsCandidate(report, *a_remote_relay.get(), false);
  ExpectReportContainsCandidate(report, *b_local.get(), true);
  ExpectReportContainsCandidate(report, *b_remote.get(), false);
}

TEST_F(RTCStatsCollectorTest, CollectRTCIceCandidatePairStats) {
  std::unique_ptr<cricket::Candidate> local_candidate = CreateFakeCandidate(
      "42.42.42.42", 42, "protocol", cricket::LOCAL_PORT_TYPE, 42);
  std::unique_ptr<cricket::Candidate> remote_candidate = CreateFakeCandidate(
      "42.42.42.42", 42, "protocol", cricket::LOCAL_PORT_TYPE, 42);

  SessionStats session_stats;

  cricket::ConnectionInfo connection_info;
  connection_info.local_candidate = *local_candidate.get();
  connection_info.remote_candidate = *remote_candidate.get();
  connection_info.writable = true;
  connection_info.sent_total_bytes = 42;
  connection_info.recv_total_bytes = 1234;
  connection_info.rtt = 1337;
  connection_info.sent_ping_requests_total = 1010;
  connection_info.recv_ping_responses = 4321;
  connection_info.sent_ping_responses = 1000;

  cricket::TransportChannelStats transport_channel_stats;
  transport_channel_stats.connection_infos.push_back(connection_info);
  session_stats.transport_stats["transport"].transport_name = "transport";
  session_stats.transport_stats["transport"].channel_stats.push_back(
      transport_channel_stats);

  // Mock the session to return the desired candidates.
  EXPECT_CALL(test_->session(), GetTransportStats(_)).WillRepeatedly(Invoke(
      [this, &session_stats](SessionStats* stats) {
        *stats = session_stats;
        return true;
      }));

  rtc::scoped_refptr<const RTCStatsReport> report = GetStatsReport();
  ExpectReportContainsCandidatePair(
      report, session_stats.transport_stats["transport"]);
}

TEST_F(RTCStatsCollectorTest, CollectRTCPeerConnectionStats) {
  int64_t before = rtc::TimeUTCMicros();
  rtc::scoped_refptr<const RTCStatsReport> report = GetStatsReport();
  int64_t after = rtc::TimeUTCMicros();
  EXPECT_EQ(report->GetStatsOfType<RTCPeerConnectionStats>().size(),
            static_cast<size_t>(1)) << "Expecting 1 RTCPeerConnectionStats.";
  const RTCStats* stats = report->Get("RTCPeerConnection");
  EXPECT_TRUE(stats);
  EXPECT_LE(before, stats->timestamp_us());
  EXPECT_LE(stats->timestamp_us(), after);
  {
    // Expected stats with no data channels
    const RTCPeerConnectionStats& pcstats =
        stats->cast_to<RTCPeerConnectionStats>();
    EXPECT_EQ(*pcstats.data_channels_opened, static_cast<uint32_t>(0));
    EXPECT_EQ(*pcstats.data_channels_closed, static_cast<uint32_t>(0));
  }

  test_->data_channels().push_back(
      new MockDataChannel(DataChannelInterface::kConnecting));
  test_->data_channels().push_back(
      new MockDataChannel(DataChannelInterface::kOpen));
  test_->data_channels().push_back(
      new MockDataChannel(DataChannelInterface::kClosing));
  test_->data_channels().push_back(
      new MockDataChannel(DataChannelInterface::kClosed));

  collector_->ClearCachedStatsReport();
  report = GetStatsReport();
  EXPECT_EQ(report->GetStatsOfType<RTCPeerConnectionStats>().size(),
            static_cast<size_t>(1)) << "Expecting 1 RTCPeerConnectionStats.";
  stats = report->Get("RTCPeerConnection");
  EXPECT_TRUE(stats);
  {
    // Expected stats with the above four data channels
    // TODO(hbos): When the |RTCPeerConnectionStats| is the number of data
    // channels that have been opened and closed, not the numbers currently
    // open/closed, we would expect opened >= closed and (opened - closed) to be
    // the number currently open. crbug.com/636818.
    const RTCPeerConnectionStats& pcstats =
        stats->cast_to<RTCPeerConnectionStats>();
    EXPECT_EQ(*pcstats.data_channels_opened, static_cast<uint32_t>(1));
    EXPECT_EQ(*pcstats.data_channels_closed, static_cast<uint32_t>(3));
  }
}

class RTCStatsCollectorTestWithFakeCollector : public testing::Test {
 public:
  RTCStatsCollectorTestWithFakeCollector()
    : test_(new rtc::RefCountedObject<RTCStatsCollectorTestHelper>()),
      collector_(FakeRTCStatsCollector::Create(
          &test_->pc(), 50 * rtc::kNumMicrosecsPerMillisec)) {
  }

 protected:
  rtc::scoped_refptr<RTCStatsCollectorTestHelper> test_;
  rtc::scoped_refptr<FakeRTCStatsCollector> collector_;
};

TEST_F(RTCStatsCollectorTestWithFakeCollector, ThreadUsageAndResultsMerging) {
  collector_->VerifyThreadUsageAndResultsMerging();
}

}  // namespace

}  // namespace webrtc
