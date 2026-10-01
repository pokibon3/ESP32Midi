// midiplay: play a Standard MIDI File to a CoreMIDI destination (default: the ESP32 synth)
//
//   midiplay song.mid             play to the first destination whose name contains "ESP32"
//   midiplay -d "IAC" song.mid    choose the destination by (partial) name
//   midiplay -l                   list destinations
import AudioToolbox
import CoreMIDI
import Foundation

func destinations() -> [(name: String, endpoint: MIDIEndpointRef)] {
  (0..<MIDIGetNumberOfDestinations()).map { i in
    let ep = MIDIGetDestination(i)
    var name: Unmanaged<CFString>?
    MIDIObjectGetStringProperty(ep, kMIDIPropertyDisplayName, &name)
    return (name?.takeRetainedValue() as String? ?? "?", ep)
  }
}

func fail(_ msg: String) -> Never {
  FileHandle.standardError.write((msg + "\n").data(using: .utf8)!)
  exit(1)
}

func check(_ status: OSStatus, _ what: String) {
  if status != noErr { fail("\(what) failed (\(status))") }
}

// ---- arguments
var args = Array(CommandLine.arguments.dropFirst())
var destQuery = "ESP32"
if args.first == "-l" {
  for d in destinations() { print(d.name) }
  exit(0)
}
if args.count >= 2 && args[0] == "-d" {
  destQuery = args[1]
  args.removeFirst(2)
}
guard args.count == 1 else { fail("usage: midiplay [-l] [-d destination] file.mid") }
let path = args[0]

guard let dest = destinations().first(where: { $0.name.localizedCaseInsensitiveContains(destQuery) }) else {
  fail("MIDI destination containing \"\(destQuery)\" not found (midiplay -l to list)")
}

// ---- direct output for reset / panic
var client = MIDIClientRef()
check(MIDIClientCreate("midiplay" as CFString, nil, nil, &client), "MIDIClientCreate")
var outPort = MIDIPortRef()
check(MIDIOutputPortCreate(client, "out" as CFString, &outPort), "MIDIOutputPortCreate")

func send(_ bytes: [UInt8]) {
  var list = MIDIPacketList()
  let pkt = MIDIPacketListInit(&list)
  _ = MIDIPacketListAdd(&list, MemoryLayout<MIDIPacketList>.size, pkt, 0, bytes.count, bytes)
  MIDISend(outPort, dest.endpoint, &list)
}

func panic() {
  for ch in 0..<16 {
    send([0xB0 | UInt8(ch), 64, 0])   // sustain off
    send([0xB0 | UInt8(ch), 123, 0])  // all notes off
    send([0xB0 | UInt8(ch), 120, 0])  // all sound off
  }
}

// ---- load sequence
var sequence: MusicSequence?
check(NewMusicSequence(&sequence), "NewMusicSequence")
guard let seq = sequence else { fail("no sequence") }
check(MusicSequenceFileLoad(seq, URL(fileURLWithPath: path) as CFURL, .midiType, []), "loading \(path)")
check(MusicSequenceSetMIDIEndpoint(seq, dest.endpoint), "MusicSequenceSetMIDIEndpoint")

var trackCount: UInt32 = 0
MusicSequenceGetTrackCount(seq, &trackCount)
var lengthBeats: MusicTimeStamp = 0
for i in 0..<trackCount {
  var track: MusicTrack?
  MusicSequenceGetIndTrack(seq, i, &track)
  var len: MusicTimeStamp = 0
  var size = UInt32(MemoryLayout<MusicTimeStamp>.size)
  MusicTrackGetProperty(track!, kSequenceTrackProperty_TrackLength, &len, &size)
  lengthBeats = max(lengthBeats, len)
}
var lengthSec: Float64 = 0
MusicSequenceGetSecondsForBeats(seq, lengthBeats, &lengthSec)

// ---- play
var player: MusicPlayer?
check(NewMusicPlayer(&player), "NewMusicPlayer")
check(MusicPlayerSetSequence(player!, seq), "MusicPlayerSetSequence")

send([0xF0, 0x7E, 0x7F, 0x09, 0x01, 0xF7])  // GM System On
usleep(100_000)

signal(SIGINT, SIG_IGN)
let sigint = DispatchSource.makeSignalSource(signal: SIGINT, queue: .main)
sigint.setEventHandler {
  MusicPlayerStop(player!)
  panic()
  print("\nstopped")
  exit(0)
}
sigint.resume()

check(MusicPlayerPreroll(player!), "MusicPlayerPreroll")
check(MusicPlayerStart(player!), "MusicPlayerStart")

func mmss(_ t: Float64) -> String { String(format: "%d:%02d", Int(t) / 60, Int(t) % 60) }
print("\((path as NSString).lastPathComponent) -> \(dest.name)  (\(mmss(lengthSec)))  Ctrl-C to stop")

Timer.scheduledTimer(withTimeInterval: 0.25, repeats: true) { _ in
  var beats: MusicTimeStamp = 0
  MusicPlayerGetTime(player!, &beats)
  var sec: Float64 = 0
  MusicSequenceGetSecondsForBeats(seq, beats, &sec)
  print("\r\(mmss(sec)) / \(mmss(lengthSec))", terminator: "")
  fflush(stdout)
  if beats >= lengthBeats {
    MusicPlayerStop(player!)
    usleep(500_000)
    panic()
    print("")
    exit(0)
  }
}
RunLoop.main.run()
