import Foundation
enum BridgeError: Error { case submission(String) }
final class Probe: @unchecked Sendable { var reads=0; var done=false; var succeeded=false }
func run(_ snapshots:[DictationSelectionSnapshot], success:Bool) {
 let p=Probe()
 DictationSelectionAwaiter.wait(requested:NSRange(location:0,length:3),previous:NSRange(location:3,length:0),attemptsRemaining:4,read:{
  let i=min(p.reads,snapshots.count-1);p.reads+=1;return snapshots[i]
 },completion:{r in p.done=true;if case .success=r {p.succeeded=true}})
 let until=Date().addingTimeInterval(2)
 while !p.done && Date()<until {RunLoop.main.run(until:Date().addingTimeInterval(0.01))}
 precondition(p.done && p.succeeded==success)
}
let good=DictationSelectionSnapshot(valueMatches:true,focusMatches:true,range:NSRange(location:0,length:3))
let stale=DictationSelectionSnapshot(valueMatches:false,focusMatches:true,range:nil)
run([stale,stale,good],success:true)
run([stale],success:false)
run([DictationSelectionSnapshot(valueMatches:true,focusMatches:false,range:NSRange(location:0,length:3))],success:false)
print("Transient read, persistent mismatch, and focus loss passed")
