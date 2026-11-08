import Foundation

struct DictationSelectionSnapshot: Sendable {
    let valueMatches: Bool
    let focusMatches: Bool
    let range: NSRange?
}

enum DictationSelectionAwaiter {
    static func wait(
        requested: NSRange, previous: NSRange,
        attemptsRemaining: Int = 12,
        pollsPerformed: Int = 0,
        read: @escaping @Sendable () -> DictationSelectionSnapshot,
        completion: @escaping @Sendable (Result<Void, Error>) -> Void
    ) {
        let snapshot = read()
        guard snapshot.focusMatches else {
            print("流式选区中止：文字一致=\(snapshot.valueMatches) 焦点一致=\(snapshot.focusMatches)")
            completion(.failure(BridgeError.submission("文字或焦点已改变，已停止流式改写")))
            return
        }
        if snapshot.valueMatches && snapshot.range == requested {
            if pollsPerformed > 0 { print("流式选区已就绪：重试 \(pollsPerformed) 次") }
            completion(.success(()))
            return
        }
        guard attemptsRemaining > 1 else {
            print("流式选区未就绪：请求=\(requested) 实际=\(String(describing: snapshot.range))")
            completion(.failure(BridgeError.submission("输入框未接受听写选区，已停止改写")))
            return
        }
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.04) {
            wait(
                requested: requested, previous: previous, attemptsRemaining: attemptsRemaining - 1,
                pollsPerformed: pollsPerformed + 1, read: read, completion: completion)
        }
    }
}
