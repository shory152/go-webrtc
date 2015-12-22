/**
 * C wrapper for the C++ PeerConnection code, to be go-compatible.
 */
#include "cpeerconnection.h"
#include "webrtc/base/common.h"
#include "talk/app/webrtc/peerconnectioninterface.h"
#include "talk/app/webrtc/test/fakeconstraints.h"
#include "talk/app/webrtc/jsepsessiondescription.h"
#include "talk/app/webrtc/webrtcsdp.h"
#include <iostream>
#include <unistd.h>
#include <future>
#include "_cgo_export.h"

#define SUCCESS 0
#define FAILURE -1
#define TIMEOUT_SECS 3

using namespace std;
using namespace webrtc;


// Smaller typedefs
typedef rtc::scoped_refptr<webrtc::PeerConnectionInterface> PC;
typedef SessionDescriptionInterface* SDP;
typedef rtc::scoped_refptr<DataChannelInterface> DataChannel;


// Peer acts as the glue between Go PeerConnection and the native
// webrtc::PeerConnectionInterface. However, it's not directly accessible
// through CGO, but indirectly through what's available in the more pure
// extern "C" header.
//
// The Go side may access this class through C.CGO_Peer.
// TODO: Better logging on the C side.
class Peer
  : public PeerConnectionObserver,
    public CreateSessionDescriptionObserver {
 public:

  // Should be called before anything else happens.
  bool Initialize() {
    promiseSDP = promise<SDP>();

    // Due to the different threading model, in order for PeerConnectionFactory
    // to be able to post async messages without getting blocked, we need to use
    // external signalling and worker thread, accounted for in this class.
    signaling_thread = new rtc::Thread();
    worker_thread = new rtc::Thread();
    signaling_thread->SetName("CGO Signalling", NULL);
    signaling_thread->Start();
    worker_thread->SetName("CGO Worker", NULL);
    worker_thread->Start();
    pc_factory = CreatePeerConnectionFactory(
      worker_thread,
      signaling_thread,
      NULL, NULL, NULL);
    if (!pc_factory.get()) {
      cout << "ERROR: Could not create PeerConnectionFactory" << endl;
      return false;
    }

    // Media constraints are hard-coded here and not exposed in Go, because
    // in this case only DTLS/SCTP data channels are desired.
    auto c = new FakeConstraints();
    c->AddOptional(MediaConstraintsInterface::kEnableDtlsSrtp, true);
    constraints = c;

    return true;
  }

  void resetPromise() {
    promiseSDP = promise<SDP>();
  }

  //
  // CreateSessionDescriptionObserver implementation
  //
  // These callbacks have been stubbed out using promises + futures, to be
  // blocking as far as Go is concerned, which allows the usage
  // of goroutines. This should be easier and more idiomatic for users.
  //
  void OnSuccess(SDP desc) {
    // cout << "[C] SDP successfully created at " << desc << endl;
    promiseSDP.set_value(desc);
  }

  void OnFailure(const std::string& error) {
    // cout << "[C] SDP Failure: " << error << endl;
    promiseSDP.set_value(NULL);
  }

  //
  // PeerConnectionObserver Implementation
  // TODO: Finish the rest of the callbacks in go.
  //
  void OnSignalingChange(PeerConnectionInterface::SignalingState state) {
    // cout << "[C] OnSignalingChange: " << state << endl;
    cgoOnSignalingStateChange(goPeerConnection, state);
  }

  // TODO: The below seems on the way to being deprecated in native code.
  /*
  void OnStateChange(PeerConnectionObserver::StateType state) {
    cout << "[C] OnStateChange: " << state << endl;
  }
  */
  void OnAddStream(webrtc::MediaStreamInterface* stream) {
    cout << "[C] OnAddStream: " << stream << endl;
    // TODO: This is required when beginning implementing Media API.
  }
  void OnRemoveStream(webrtc::MediaStreamInterface* stream) {
    cout << "[C] OnRemoveStream: " << stream << endl;
    // TODO: This is required when beginning implementing Media API.
  }

  void OnRenegotiationNeeded() {
    cout << "[C] OnRenegotiationNeeded" << endl;
    cgoOnNegotiationNeeded(goPeerConnection);
  }

  void OnIceCandidate(const IceCandidateInterface* ic) {
    // cout << "[C] OnIceCandidate" << ic << endl;
    std::string sdp;
    ic->ToString(&sdp);
    CGO_IceCandidate cgoIC = {
      const_cast<char*>(ic->sdp_mid().c_str()),
      ic->sdp_mline_index(),
      const_cast<char*>(sdp.c_str())
    };
    cgoOnIceCandidate(goPeerConnection, cgoIC);
  }

  void OnIceComplete() {
    cgoOnIceComplete(goPeerConnection);
  }

  void OnDataChannel(DataChannelInterface* data_channel) {
    // cout << "[C] OnDataChannel: " << data_channel << endl;
    data_channel->AddRef();
    cgoOnDataChannel(goPeerConnection, data_channel);
  }

  PeerConnectionInterface::RTCConfiguration *config;
  PeerConnectionInterface::RTCOfferAnswerOptions options;
  const MediaConstraintsInterface* constraints;

  PC pc_;                  // Pointer to internal PeerConnection
  void *goPeerConnection;  // External GO PeerConnection

  // Pass SDPs through promises instead of callbacks, to allow benefits
  // as described above.
  promise<SDP> promiseSDP;
  // However, this has the effect that CreateOffer and CreateAnswer must not be
  // concurrent, to themselves or each other (which isn't expected anyways) due
  // to the simplistic way in which futures are used here.

  rtc::scoped_refptr<PeerConnectionFactoryInterface> pc_factory;
  // TODO: prepare and expose IceServers for real.
  // PeerConnectionInterface::IceServers ice_servers;

  DataChannel channel;

 protected:
  rtc::Thread *signaling_thread;
  rtc::Thread *worker_thread;

};  // class Peer

// Keep track of Peers in global scope to prevent deallocation, due to the
// required scoped_refptr from implementing the Observer interface.
vector<rtc::scoped_refptr<Peer>> localPeers;


class PeerSDPObserver : public SetSessionDescriptionObserver {
 public:
  static PeerSDPObserver* Create() {
    return new rtc::RefCountedObject<PeerSDPObserver>();
  }
  virtual void OnSuccess() {
    // cout << "[C] SDP Set Success!" << endl;
    promiseSet.set_value(0);
  }
  virtual void OnFailure(const std::string& error) {
    cout << "[C ERROR] SessionDescription: " << error << endl;
    promiseSet.set_value(-1);
  }
  promise<int> promiseSet = promise<int>();

 protected:
  PeerSDPObserver() {}
  ~PeerSDPObserver() {}

};  // class PeerSDPObserver


//
// extern "C" Go-accessible functions.
//

// Create and return the Peer object, which provides initial native code
// glue for the PeerConnection constructor.
CGO_Peer CGO_InitializePeer(void *goPc) {
  rtc::scoped_refptr<Peer> localPeer = new rtc::RefCountedObject<Peer>();
  localPeer->Initialize();
  localPeers.push_back(localPeer);
  // Reference to external Go PeerConnection struct is required for firing
  // callbacks correctly.
  localPeer->goPeerConnection = goPc;
  return localPeer;
}

// This helper converts RTCConfiguration struct from GO to C++.
PeerConnectionInterface::RTCConfiguration *castConfig_(
    CGO_Configuration *cgoConfig) {
  PeerConnectionInterface::RTCConfiguration* c =
      new PeerConnectionInterface::RTCConfiguration();

  vector<CGO_IceServer> servers( cgoConfig->iceServers,
      cgoConfig->iceServers + cgoConfig->numIceServers);
  // Pass in all IceServer structs for PeerConnectionInterface.
  for (auto s : servers) {
    // cgo only allows C arrays, but webrtc expects std::vectors
    vector<string> urls(s.urls, s.urls + s.numUrls);
    c->servers.push_back({
      "",  // TODO: Remove once webrtc deprecates the first uri field.
      urls,
      s.username,
      s.credential
    });
  }

  // Cast Go const "enums" to C++ Enums.
  c->type = (PeerConnectionInterface::IceTransportsType)
      cgoConfig->iceTransportPolicy;
  c->bundle_policy = (PeerConnectionInterface::
      BundlePolicy)cgoConfig->bundlePolicy;

  // TODO: [ED] extensions.
  // c->rtcp_mux_policy = (PeerConnectionInterface::
      // RtcpMuxPolicy)cgoConfig->RtcpMuxPolicy;
  return c;
}

// |Peer| method: create a native code PeerConnection object.
// Returns 0 on Success.
int CGO_CreatePeerConnection(CGO_Peer cgoPeer, CGO_Configuration *cgoConfig) {
  Peer *peer = (Peer*)cgoPeer;
  peer->config = castConfig_(cgoConfig);

  // Prepare a native PeerConnection object.
  peer->pc_ = peer->pc_factory->CreatePeerConnection(
    *peer->config,
    peer->constraints,
    NULL,  // port allocator      (reasonable default already within)
    NULL,  // dtls identity store (reasonable default already within)
    peer   // "observer"
    );

  if (!peer->pc_.get()) {
    cout << "ERROR: Could not create PeerConnection." << endl;
    return FAILURE;
  }
  return SUCCESS;
}

bool SDPtimeout(future<SDP> *f, int seconds) {
  auto status = f->wait_for(chrono::seconds(TIMEOUT_SECS));
  return future_status::ready != status;
}

// PeerConnection::CreateOffer
// Blocks until libwebrtc succeeds in generating the SDP offer,
// @returns SDP (pointer), or NULL on timeeout.
CGO_sdp CGO_CreateOffer(CGO_Peer cgoPeer) {
  // TODO: Provide an actual RTCOfferOptions as an argument.
  Peer* peer = (Peer*)cgoPeer;
  auto r = peer->promiseSDP.get_future();
  peer->pc_->CreateOffer(peer, peer->constraints);
  if (SDPtimeout(&r, TIMEOUT_SECS)) {
    cout << "[C] CreateOffer timed out after " << TIMEOUT_SECS << endl;
    peer->resetPromise();
    return NULL;
  }
  SDP sdp = r.get();  // blocking
  peer->resetPromise();
  return (CGO_sdp)sdp;
}


// PeerConnection::CreateAnswer
// Blocks until libwebrtc succeeds in generating the SDP answer.
// @returns SDP, or NULL on timeout.
CGO_sdp CGO_CreateAnswer(CGO_Peer cgoPeer) {
  Peer *peer = (Peer*)cgoPeer;
  cout << "[C] CreateAnswer" << peer << endl;
  auto r = peer->promiseSDP.get_future();
  peer->pc_->CreateAnswer(peer, peer->constraints);
  if (SDPtimeout(&r, TIMEOUT_SECS)) {
    cout << "[C] CreateAnswer timed out after " << TIMEOUT_SECS << endl;
    peer->resetPromise();
    return NULL;
  }
  SDP sdp = r.get();  // blocking
  peer->resetPromise();
  return (CGO_sdp)sdp;
}


// Serialize SDP message to a string Go can use.
CGO_sdpString CGO_SerializeSDP(CGO_sdp sdp) {
  auto s = new string();
  SDP cSDP = (SDP)sdp;
  cSDP->ToString(s);
  return (CGO_sdpString)s->c_str();
}

// Given a fully serialized SDP string |msg|, return a CGO sdp object.
CGO_sdp CGO_DeserializeSDP(const char *type, const char *msg) {
  // TODO: Look into type.
  auto jsep_sdp = new JsepSessionDescription(type);
  SdpParseError err;
  auto msg_str = new string(msg);
  SdpDeserialize(*msg_str, jsep_sdp, &err);
  return (CGO_sdp)jsep_sdp;
}

// PeerConnection::SetLocalDescription
int CGO_SetLocalDescription(CGO_Peer cgoPeer, CGO_sdp sdp) {
  PC cPC = ((Peer*)cgoPeer)->pc_;
  auto obs = PeerSDPObserver::Create();
  auto r = obs->promiseSet.get_future();
  cPC->SetLocalDescription(obs, (SDP)sdp);
  return r.get();
}

// PeerConnection::SetRemoteDescription
int CGO_SetRemoteDescription(CGO_Peer cgoPeer, CGO_sdp sdp) {
  PC cPC = ((Peer*)cgoPeer)->pc_;
  auto obs = PeerSDPObserver::Create();
  auto r = obs->promiseSet.get_future();
  cPC->SetRemoteDescription(obs, (SDP)sdp);
  return r.get();
}

// PeerConnection::AddIceCandidate
int CGO_AddIceCandidate(CGO_Peer cgoPeer, CGO_IceCandidate *cgoIC) {
  PC cPC = ((Peer*)cgoPeer)->pc_;
  SdpParseError *error = nullptr;
  IceCandidateInterface *ic = webrtc::CreateIceCandidate(
    string(cgoIC->sdp_mid), cgoIC->sdp_mline_index, string(cgoIC->sdp), error);
  if (error || !ic) {
    cout << "[C] SDP parse error." << endl;
    return FAILURE;
  }
  if (!cPC->AddIceCandidate(ic)) {
    cout << "[C] problem adding ICE candidate." << endl;
    return FAILURE;
  }
  return SUCCESS;
}

// PeerConnection::signaling_state
int CGO_GetSignalingState(CGO_Peer pc) {
  PC cPC = ((Peer*)pc)->pc_;
  return cPC->signaling_state();
}

// PeerConnection::SetConfiguration
int CGO_SetConfiguration(CGO_Peer cgoPeer, CGO_Configuration* cgoConfig) {
  Peer *peer = (Peer*)cgoPeer;
  auto cConfig = castConfig_(cgoConfig);
  bool success = peer->pc_->SetConfiguration(*cConfig);
  if (success) {
    peer->config = cConfig;
    return SUCCESS;
  }
  return FAILURE;
}

// PeerConnection::CreateDataChannel
CGO_Channel CGO_CreateDataChannel(CGO_Peer cgoPeer, char *label, void *dict) {
  auto cPeer = (Peer*)cgoPeer;
  DataChannelInit *r = (DataChannelInit*)dict;
  // TODO: a real config struct, with correct fields
  DataChannelInit config;
  string *l = new string(label);
  // This is a ref_ptr, and needs to be kept track of.
  // TODO: Keep track of a vector of these internally.
  auto channel = cPeer->pc_->CreateDataChannel(*l, &config);
  cPeer->channel = channel;
  cout << "[C] Created data channel: " << channel << endl;
  webrtc::DataChannelInterface* c = cPeer->channel.get();
  return c;
}

// PeerConnection::Close
void CGO_Close(CGO_Peer peer) {
  auto cPeer = (Peer*)peer;
  cPeer->pc_->Close();
  cout << "[C] Closed PeerConnection." << endl;
}
