import AppKit
import ApplicationServices
import Foundation

struct DictationDraft: Equatable {
    private(set) var expectedValue: String
    private(set) var insertionRange: NSRange

    init(value: String, selection: NSRange) throws {
        guard selection.location >= 0, selection.length >= 0, selection.location <= (value as NSString).length,
            selection.length <= (value as NSString).length - selection.location
        else { throw BridgeError.submission("输入框选区无效") }
        expectedValue = value
        insertionRange = selection
    }

    func replacing(with text: String, currentValue: String) throws -> Self {
        guard currentValue == expectedValue else {
            throw BridgeError.submission("输入框内容持续不一致，已暂停保护草稿")
        }
        var next = self
        next.expectedValue = (expectedValue as NSString).replacingCharacters(in: insertionRange, with: text)
        next.insertionRange.length = (text as NSString).length
        return next
    }
    var caret: NSRange { NSRange(location: NSMaxRange(insertionRange), length: 0) }
}

struct DictationEditor: @unchecked Sendable {
    let element: AXUIElement
    let processID: pid_t
}

final class LiveDictationInput: @unchecked Sendable {
    private final class Session: @unchecked Sendable {
        let editor: DictationEditor
        var draft: DictationDraft
        init(editor: DictationEditor, draft: DictationDraft) {
            self.editor = editor
            self.draft = draft
        }
    }
    private var sessions: [UUID: Session] = [:]
    private var failures: [UUID: String] = [:]
    func isPaused(id: UUID) -> Bool { failures[id] != nil }

    func begin(id: UUID, completion: @escaping @Sendable (Result<Void, Error>) -> Void) {
        CodexSubmitter().resolveDictationEditor { result in
            do {
                let editor = try result.get()
                guard let value = Self.value(of: editor.element), let selection = Self.selection(of: editor.element), selection.length == 0 else {
                    throw BridgeError.submission("请在 Codex 输入框放置光标，不要选择已有文字")
                }
                self.sessions[id] = Session(editor: editor, draft: try DictationDraft(value: value, selection: selection))
                completion(.success(()))
            } catch { completion(.failure(error)) }
        }
    }
    func send(id: UUID, completion: @escaping @Sendable (Result<Void, Error>) -> Void) {
        guard let session = sessions[id], failures[id] == nil,
            NSWorkspace.shared.frontmostApplication?.processIdentifier == session.editor.processID,
            Self.isFocused(session.editor), Self.value(of: session.editor.element) == session.draft.expectedValue,
            Self.selection(of: session.editor.element) == session.draft.caret,
            session.draft.insertionRange.length > 0 else {
            completion(.failure(BridgeError.submission("输入框或光标已变化，未发送"))); return
        }
        // Codex supports Command-Return independently of its multiline Enter behavior.
        // One key pair only. A timeout never retries this external action.
        for down in [true, false] {
            let event = CGEvent(keyboardEventSource: nil, virtualKey: 36, keyDown: down)
            event?.flags = .maskCommand
            event?.post(tap: .cghidEventTap)
        }
        DispatchQueue.main.asyncAfter(deadline: .now()+0.8) {
            if Self.value(of: session.editor.element)?.isEmpty == true {
                self.end(id:id); completion(.success(()))
            } else { completion(.failure(BridgeError.submission("已执行发送，但未确认输入框清空；请查看 Mac，不会自动重发"))) }
        }
    }
    func end(id: UUID) {
        sessions.removeValue(forKey: id)
        failures.removeValue(forKey: id)
    }

    func update(id: UUID, text: String, isFinal: Bool, completion: @escaping @Sendable (Result<Void, Error>) -> Void) {
        precondition(Thread.isMainThread)
        let finish: @Sendable (Result<Void, Error>) -> Void = { result in
            if case .failure(let error) = result { self.failures[id] = error.localizedDescription }
            if case .failure = result, let session = self.sessions[id],
                Self.value(of: session.editor.element) == session.draft.expectedValue,
                Self.selection(of: session.editor.element) == session.draft.insertionRange
            {
                _ = Self.select(session.draft.caret, in: session.editor.element)
            }
            if isFinal {
                self.sessions.removeValue(forKey: id)
                self.failures.removeValue(forKey: id)
                if case .failure = result {
                    NSPasteboard.general.clearContents()
                    NSPasteboard.general.setString(text, forType: .string)
                }
            }
            completion(result)
        }
        if let failure = failures[id] { finish(.failure(BridgeError.submission(failure))); return }
        if let session = sessions[id] {
            apply(text: text, to: session, completion: finish)
            return
        }
        CodexSubmitter().resolveDictationEditor { result in
            do {
                let editor = try result.get()
                guard let value = Self.value(of: editor.element), let selection = Self.selection(of: editor.element)
                else {
                    throw BridgeError.submission("当前输入框不支持安全的流式选区更新")
                }
                let session = Session(editor: editor, draft: try DictationDraft(value: value, selection: selection))
                self.sessions[id] = session
                self.apply(text: text, to: session, completion: finish)
            } catch { finish(.failure(error)) }
        }
    }

    private func apply(text: String, to session: Session, attemptsRemaining: Int = 15, completion: @escaping @Sendable (Result<Void, Error>) -> Void)
    {
        do {
            guard NSWorkspace.shared.frontmostApplication?.processIdentifier == session.editor.processID,
                Self.isFocused(session.editor) else {
                throw BridgeError.submission("输入焦点已改变，已暂停流式输入")
            }
            let observed = Self.value(of: session.editor.element)
            let range = Self.selection(of: session.editor.element)
            if (observed != session.draft.expectedValue || (range != session.draft.caret && range != session.draft.insertionRange)), attemptsRemaining > 1 {
                DispatchQueue.main.asyncAfter(deadline: .now() + 0.1) {
                    self.apply(text: text, to: session, attemptsRemaining: attemptsRemaining - 1, completion: completion)
                }
                return
            }
            guard let value = observed else {
                throw BridgeError.submission("无法读取输入框文字，已停止改写")
            }
            let next = try session.draft.replacing(with: text, currentValue: value)
            if value == next.expectedValue { completion(.success(())); return }
            guard NSWorkspace.shared.frontmostApplication?.processIdentifier == session.editor.processID,
                Self.isFocused(session.editor),
                let selection = Self.selection(of: session.editor.element),
                selection == session.draft.caret || selection == session.draft.insertionRange
            else { throw BridgeError.submission("输入焦点或光标已改变，已暂停流式输入") }
            guard Self.select(session.draft.insertionRange, in: session.editor.element) else {
                throw BridgeError.submission("无法选择本次听写文字，已停止改写")
            }
            DictationSelectionAwaiter.wait(
                requested: session.draft.insertionRange, previous: selection,
                read: {
                    DictationSelectionSnapshot(
                        valueMatches: Self.value(of: session.editor.element) == value,
                        focusMatches: NSWorkspace.shared.frontmostApplication?.processIdentifier
                            == session.editor.processID && Self.isFocused(session.editor),
                        range: Self.selection(of: session.editor.element)
                    )
                },
                completion: { result in
                    switch result {
                    case .success: self.paste(text: text, next: next, to: session, completion: completion)
                    case .failure(let error): completion(.failure(error))
                    }
                }
            )
        } catch { completion(.failure(error)) }
    }

    private func paste(
        text: String, next: DictationDraft, to session: Session,
        completion: @escaping @Sendable (Result<Void, Error>) -> Void
    ) {
        guard NSWorkspace.shared.frontmostApplication?.processIdentifier == session.editor.processID,
            Self.isFocused(session.editor), Self.value(of: session.editor.element) == session.draft.expectedValue,
            Self.selection(of: session.editor.element) == session.draft.insertionRange
        else { completion(.failure(BridgeError.submission("选区或焦点已变化，已停止流式改写"))); return }
        let snapshot = PasteboardSnapshot.capture()
        let pasteboard = NSPasteboard.general
        pasteboard.clearContents()
        guard pasteboard.setString(text, forType: .string) else {
            completion(.failure(BridgeError.submission("无法写入听写文字")))
            return
        }
        let count = pasteboard.changeCount
        let source = CGEventSource(stateID: .hidSystemState)
        for down in [true, false] {
            let event = CGEvent(keyboardEventSource: source, virtualKey: text.isEmpty ? 51 : 9, keyDown: down)
            event?.flags = text.isEmpty ? [] : .maskCommand
            event?.post(tap: .cghidEventTap)
        }
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.25) {
            self.confirm(next, in: session) { result in
                snapshot.restore(ifUnchangedSince: count)
                completion(result)
            }
        }
    }

    private func confirm(
        _ next: DictationDraft, in session: Session, attemptsRemaining: Int = 16, stableReads: Int = 0,
        completion: @escaping @Sendable (Result<Void, Error>) -> Void
    ) {
        let actual = Self.value(of: session.editor.element)
        if actual == next.expectedValue && stableReads >= 1 {
            session.draft = next
            completion(.success(()))
        } else if attemptsRemaining > 1,
            NSWorkspace.shared.frontmostApplication?.processIdentifier == session.editor.processID,
            Self.isFocused(session.editor) {
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.1) {
                self.confirm(next, in: session, attemptsRemaining: attemptsRemaining - 1,
                    stableReads: actual == next.expectedValue ? stableReads + 1 : 0, completion: completion)
            }
        } else {
            print(
                "流式读回不一致：原长度=\((session.draft.expectedValue as NSString).length) 期望长度=\((next.expectedValue as NSString).length) 实际长度=\(actual.map { ($0 as NSString).length } ?? -1) 实际选区=\(String(describing: Self.selection(of: session.editor.element)))"
            )
            completion(.failure(BridgeError.submission("输入框未接受流式文字，已停止改写")))
        }
    }

    private static func value(of element: AXUIElement) -> String? {
        AccessibleText.value(of: element)
    }
    private static func selection(of element: AXUIElement) -> NSRange? {
        var value: CFTypeRef?
        guard AXUIElementCopyAttributeValue(element, kAXSelectedTextRangeAttribute as CFString, &value) == .success,
            let value, CFGetTypeID(value) == AXValueGetTypeID()
        else { return nil }
        var range = CFRange()
        guard AXValueGetValue(value as! AXValue, .cfRange, &range), range.location >= 0, range.length >= 0 else {
            return nil
        }
        return NSRange(location: range.location, length: range.length)
    }
    private static func select(_ range: NSRange, in element: AXUIElement) -> Bool {
        var value = CFRange(location: range.location, length: range.length)
        guard let boxed = AXValueCreate(.cfRange, &value) else { return false }
        return AXUIElementSetAttributeValue(element, kAXSelectedTextRangeAttribute as CFString, boxed) == .success
    }
    private static func isFocused(_ editor: DictationEditor) -> Bool {
        var value: CFTypeRef?
        if AXUIElementCopyAttributeValue(editor.element, kAXFocusedAttribute as CFString, &value) == .success,
            value as? Bool == true
        {
            return true
        }
        let app = AXUIElementCreateApplication(editor.processID)
        return AXUIElementCopyAttributeValue(app, kAXFocusedUIElementAttribute as CFString, &value) == .success
            && value.map { CFEqual($0, editor.element) } == true
    }
}
