import AppKit
import Foundation
import ApplicationServices
import Darwin

enum BridgeError: LocalizedError {
    case submission(String)
    var errorDescription: String? { if case .submission(let s) = self { return s }; return nil }
}
if CommandLine.arguments.contains("--permission") {
    print(AXIsProcessTrusted() ? "authorized" : "needs-accessibility"); exit(0)
}
if CommandLine.arguments.contains("--authorize") {
    _ = AXIsProcessTrustedWithOptions(["AXTrustedCheckOptionPrompt":true] as CFDictionary); exit(0)
}
let input = LiveDictationInput()
let app = NSApplication.shared
app.setActivationPolicy(.accessory)
let directory=FileManager.default.homeDirectoryForCurrentUser.appendingPathComponent("Library/Application Support/MosaicoCodexPet/voice")
let socketPath=directory.appendingPathComponent("input.sock").path
let fd=socket(AF_UNIX,SOCK_STREAM,0)
guard fd>=0 else {exit(1)}
var address=sockaddr_un();address.sun_family=sa_family_t(AF_UNIX)
let bytes=Array(socketPath.utf8)+[0]
guard bytes.count<=MemoryLayout.size(ofValue:address.sun_path) else {exit(1)}
withUnsafeMutableBytes(of:&address.sun_path) {$0.copyBytes(from:bytes)}
address.sun_len=UInt8(MemoryLayout<sockaddr_un>.size)
unlink(socketPath)
let bound=withUnsafePointer(to:&address) {ptr in
    ptr.withMemoryRebound(to:sockaddr.self,capacity:1) {Darwin.bind(fd,$0,socklen_t(MemoryLayout<sockaddr_un>.size))}
}
guard bound==0,listen(fd,1)==0 else {exit(1)}
chmod(socketPath,0o600)
@Sendable func reply(_ handle:FileHandle,_ id:String,_ result:Result<Void,Error>) {
    var response:[String:Any]=["id":id,"ok":true]
    if case .failure(let error)=result {response["ok"]=false;response["error"]=error.localizedDescription}
    if var data=try? JSONSerialization.data(withJSONObject:response) {data.append(10);try? handle.write(contentsOf:data)}
}
DispatchQueue.global().async {
    while true {
        let client=accept(fd,nil,nil);if client<0 {continue}
        var noSignal:Int32=1;setsockopt(client,SOL_SOCKET,SO_NOSIGPIPE,&noSignal,socklen_t(MemoryLayout<Int32>.size))
        let handle=FileHandle(fileDescriptor:client,closeOnDealloc:true)
        var pending=Data()
        while true {
            let chunk=handle.availableData
            if chunk.isEmpty {break}
            pending.append(chunk)
            if pending.count>16384 {break}
            while let end=pending.firstIndex(of:10) {
                let data=Data(pending.prefix(upTo:end));pending.removeSubrange(...end)
                guard let request=try? JSONSerialization.jsonObject(with:data) as? [String:String],let key=request["id"],let id=UUID(uuidString:request["session"] ?? "") else {continue}
                DispatchQueue.main.async {
                    switch request["action"] {
                    case "begin":input.begin(id:id) {reply(handle,key,$0)}
                    case "update":input.update(id:id,text:request["text"] ?? "",isFinal:false) {reply(handle,key,$0)}
                    case "send":input.send(id:id) {reply(handle,key,$0)}
                    case "end":input.end(id:id);reply(handle,key,.success(()))
                    default:reply(handle,key,.failure(BridgeError.submission("Unknown action")))
                    }
                }
            }
        }
        try? handle.close()
    }
}
app.run()
