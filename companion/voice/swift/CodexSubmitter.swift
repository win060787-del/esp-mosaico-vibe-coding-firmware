// Copyright (c) 2026 FanXeon@Poemcoder with Codex
import AppKit
import ApplicationServices
import Foundation

enum CodexActivationResult: Equatable {
    case activated
    case launchRequested
    case unavailable
}

enum CodexTaskDirection: Equatable {
    case previous
    case next

    var menuItemTitles: [String] {
        switch self {
        case .previous: return ["Previous Task", "上一个任务", "上一个会话", "Previous Sidebar Item", "上一个侧边栏项目", "上一個側邊欄項目"]
        case .next: return ["Next Task", "下一个任务", "下一个会话", "Next Sidebar Item", "下一个侧边栏项目", "下一個側邊欄項目"]
        }
    }
}

struct CodexSubmitter {
    static let accessibilityLaunchArgument = "--force-renderer-accessibility"

    private let bundleIdentifier = "com.openai.codex"

    static func sanitizedLaunchEnvironment(
        _ environment: [String: String] = ProcessInfo.processInfo.environment
    ) -> [String: String] {
        MiAoProcessEnvironment.sanitizedForExternalProcess(environment)
    }

    func submit(
        _ text: String,
        force: Bool,
        focusAttemptsRemaining: Int = 3,
        completion: @escaping @Sendable (Result<Void, Error>) -> Void
    ) {
        guard Thread.isMainThread else {
            DispatchQueue.main.async {
                submit(text, force: force, focusAttemptsRemaining: focusAttemptsRemaining, completion: completion)
            }
            return
        }

        guard !text.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty else {
            completion(.failure(BridgeError.submission("转写为空，已取消发送")))
            return
        }

        let options = ["AXTrustedCheckOptionPrompt": true] as CFDictionary
        guard AXIsProcessTrustedWithOptions(options) else {
            NSPasteboard.general.clearContents()
            NSPasteboard.general.setString(text, forType: .string)
            completion(
                .failure(
                    BridgeError.submission("尚未授予辅助功能权限；transcript 已复制到剪贴板")
                )
            )
            return
        }

        guard let app = NSRunningApplication.runningApplications(withBundleIdentifier: bundleIdentifier).first else {
            NSPasteboard.general.clearContents()
            NSPasteboard.general.setString(text, forType: .string)
            completion(.failure(BridgeError.submission("Codex 未运行；transcript 已复制到剪贴板")))
            return
        }

        app.activate(options: [.activateAllWindows])
        prepareAccessibility(for: app)
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.35) {
            if !force {
                let applicationElement = AXUIElementCreateApplication(app.processIdentifier)
                guard focusCodexEditorWithAccessibility(in: applicationElement) else {
                    if focusAttemptsRemaining > 1 {
                        DispatchQueue.main.asyncAfter(deadline: .now() + 0.15) {
                            submit(
                                text, force: force, focusAttemptsRemaining: focusAttemptsRemaining - 1,
                                completion: completion)
                        }
                        return
                    }
                    copyOnly(text)
                    completion(
                        .failure(
                            BridgeError.submission(
                                "无法确认唯一输入框的焦点；文字已复制到剪贴板，可手动粘贴"
                            )
                        )
                    )
                    return
                }
            }

            let snapshot = PasteboardSnapshot.capture()
            let pasteboard = NSPasteboard.general
            pasteboard.clearContents()
            guard pasteboard.setString(text, forType: .string) else {
                snapshot.restore(ifUnchangedSince: pasteboard.changeCount)
                completion(.failure(BridgeError.submission("无法写入剪贴板，已取消发送")))
                return
            }
            let injectedChangeCount = pasteboard.changeCount
            postKey(keyCode: 9, flags: .maskCommand)  // Cmd+V
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.35) {
                snapshot.restore(ifUnchangedSince: injectedChangeCount)
                completion(.success(()))
            }
        }
    }

    @discardableResult
    func activateCodex() -> Bool {
        guard let app = NSRunningApplication.runningApplications(withBundleIdentifier: bundleIdentifier).first else {
            return false
        }
        return app.activate(options: [.activateAllWindows])
    }

    func resolveDictationEditor(
        attemptsRemaining: Int = 3, completion: @escaping @Sendable (Result<DictationEditor, Error>) -> Void
    ) {
        guard AXIsProcessTrusted(),
            let app = NSRunningApplication.runningApplications(withBundleIdentifier: bundleIdentifier).first
        else {
            completion(.failure(BridgeError.submission("Codex 未运行或遥声未获辅助功能授权")))
            return
        }
        if NSWorkspace.shared.frontmostApplication?.processIdentifier == app.processIdentifier {
            let element = AXUIElementCreateApplication(app.processIdentifier)
            if let candidate = preferredEditor(in: element) {
                let focused = elementAttribute(element, kAXFocusedUIElementAttribute)
                if booleanAttribute(candidate, kAXFocusedAttribute) == true
                    || focused.map({ CFEqual($0, candidate) }) == true
                {
                    completion(.success(DictationEditor(element: candidate, processID: app.processIdentifier)))
                    return
                }
            }
        }
        app.activate(options: [.activateAllWindows])
        prepareAccessibility(for: app)
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.2) {
            let element = AXUIElementCreateApplication(app.processIdentifier)
            if focusCodexEditorWithAccessibility(in: element) {
                if let candidate = preferredEditor(in: element) {
                    completion(.success(DictationEditor(element: candidate, processID: app.processIdentifier)))
                    return
                }
            }
            if attemptsRemaining > 1 {
                DispatchQueue.main.asyncAfter(deadline: .now() + 0.15) {
                    resolveDictationEditor(attemptsRemaining: attemptsRemaining - 1, completion: completion)
                }
            } else {
                completion(.failure(BridgeError.submission("无法确认唯一输入框的焦点")))
            }
        }
    }

    func launchOrActivateCodex() -> CodexActivationResult {
        if activateCodex() { return .activated }
        guard let url = NSWorkspace.shared.urlForApplication(withBundleIdentifier: bundleIdentifier)
        else { return .unavailable }

        let configuration = NSWorkspace.OpenConfiguration()
        configuration.activates = true
        configuration.arguments = [Self.accessibilityLaunchArgument]
        configuration.environment = Self.sanitizedLaunchEnvironment()
        NSWorkspace.shared.openApplication(at: url, configuration: configuration) { _, error in
            if let error {
                fputs("启动 Codex 失败：\(error.localizedDescription)\n", stderr)
            }
        }
        return .launchRequested
    }

    func editorDiagnostics() -> [String] {
        guard
            let app = NSRunningApplication.runningApplications(withBundleIdentifier: bundleIdentifier)
                .first
        else { return ["Codex 未运行"] }
        let applicationElement = AXUIElementCreateApplication(app.processIdentifier)
        let searchRoot = elementAttribute(applicationElement, kAXFocusedWindowAttribute) ?? applicationElement
        let focused = elementAttribute(applicationElement, kAXFocusedUIElementAttribute)
        let candidates = findTextInputs(in: searchRoot)
        var lines = ["候选输入控件：\(candidates.count)"]
        if let focused {
            lines.append("当前焦点：\(describe(element: focused))")
        } else {
            lines.append("当前焦点：不可读取")
        }
        lines.append(
            contentsOf: candidates.enumerated().map { index, element in
                "候选 \(index + 1)：\(describe(element: element))"
            })
        return lines
    }

    @discardableResult
    func navigateTask(_ direction: CodexTaskDirection) -> Bool {
        guard
            let app = NSRunningApplication.runningApplications(withBundleIdentifier: bundleIdentifier)
                .first
        else { return false }
        prepareAccessibility(for: app)
        let applicationElement = AXUIElementCreateApplication(app.processIdentifier)
        guard
            let menuBar = elementAttribute(applicationElement, kAXMenuBarAttribute),
            let menuItem = findMenuItem(in: menuBar, titles: direction.menuItemTitles)
        else { return false }
        app.activate(options: [.activateAllWindows])
        return AXUIElementPerformAction(menuItem, kAXPressAction as CFString) == .success
    }

    private func prepareAccessibility(for app: NSRunningApplication) {
        // Electron's supported runtime API; no restart or debug port is needed.
        // Failure still leaves all normal editor and focus checks in place.
        let element = AXUIElementCreateApplication(app.processIdentifier)
        _ = AXUIElementSetAttributeValue(element, "AXManualAccessibility" as CFString, kCFBooleanTrue)
    }

    private func focusCodexEditorWithAccessibility(in applicationElement: AXUIElement) -> Bool {
        guard let candidate = preferredEditor(in: applicationElement) else {
            print("输入框校验失败：多个输入框且无法确认当前焦点")
            return false
        }
        let result = AXUIElementSetAttributeValue(
            candidate,
            kAXFocusedAttribute as CFString,
            kCFBooleanTrue
        )
        let focused = elementAttribute(applicationElement, kAXFocusedUIElementAttribute)
        let focusMatches = focused.map { CFEqual($0, candidate) } ?? false
        let candidateFocused = booleanAttribute(candidate, kAXFocusedAttribute) == true
        let frontmost = NSWorkspace.shared.frontmostApplication?.bundleIdentifier == bundleIdentifier
        let verified = CodexFocusPolicy.isVerified(
            candidateCount: 1,
            focusMatches: focusMatches,
            candidateFocused: candidateFocused,
            targetIsFrontmost: frontmost
        )
        if !verified {
            print("输入框焦点校验失败：set=\(result.rawValue) focused=\(candidateFocused) frontmost=\(frontmost)")
        }
        return verified
    }

    private func preferredEditor(in applicationElement: AXUIElement) -> AXUIElement? {
        let root = elementAttribute(applicationElement, kAXFocusedWindowAttribute) ?? applicationElement
        let candidates = findTextInputs(in: root)
        let focused = elementAttribute(applicationElement, kAXFocusedUIElementAttribute)
        let focusedIndices = candidates.indices.filter { index in
            booleanAttribute(candidates[index], kAXFocusedAttribute) == true
                || focused.map { CFEqual($0, candidates[index]) } == true
        }
        guard
            let index = CodexEditorSelectionPolicy.index(
                candidateCount: candidates.count, focusedIndices: focusedIndices)
        else { return nil }
        return candidates[index]
    }

    private func findTextInputs(in root: AXUIElement) -> [AXUIElement] {
        var queue: [(element: AXUIElement, depth: Int)] = [(root, 0)]
        var cursor = 0
        var matches: [AXUIElement] = []

        while cursor < queue.count, cursor < 10_000 {
            let current = queue[cursor]
            cursor += 1

            if isAcceptedTextInput(current.element), isEnabled(current.element) {
                matches.append(current.element)
            }
            guard current.depth < 50 else { continue }
            for child in elementArrayAttribute(current.element, kAXChildrenAttribute) {
                queue.append((child, current.depth + 1))
            }
        }
        return matches
    }

    private func findMenuItem(in root: AXUIElement, titles: [String]) -> AXUIElement? {
        var queue: [(element: AXUIElement, depth: Int)] = [(root, 0)]
        var cursor = 0

        while cursor < queue.count, cursor < 1_000 {
            let current = queue[cursor]
            cursor += 1
            if stringAttribute(current.element, kAXRoleAttribute) == kAXMenuItemRole as String,
                let title = stringAttribute(current.element, kAXTitleAttribute),
                titles.contains(title)
            {
                return current.element
            }
            guard current.depth < 5 else { continue }
            for child in elementArrayAttribute(current.element, kAXChildrenAttribute) {
                queue.append((child, current.depth + 1))
            }
        }
        return nil
    }

    private func isAcceptedTextInput(_ element: AXUIElement) -> Bool {
        guard let role = stringAttribute(element, kAXRoleAttribute) else { return false }
        return [kAXTextAreaRole as String, kAXTextFieldRole as String].contains(role)
    }

    private func isEnabled(_ element: AXUIElement) -> Bool {
        var value: CFTypeRef?
        let result = AXUIElementCopyAttributeValue(
            element,
            kAXEnabledAttribute as CFString,
            &value
        )
        return result != .success || (value as? Bool) != false
    }

    private func elementAttribute(_ element: AXUIElement, _ attribute: String) -> AXUIElement? {
        var value: CFTypeRef?
        guard
            AXUIElementCopyAttributeValue(element, attribute as CFString, &value) == .success,
            let value,
            CFGetTypeID(value) == AXUIElementGetTypeID()
        else { return nil }
        return (value as! AXUIElement)
    }

    private func elementArrayAttribute(_ element: AXUIElement, _ attribute: String) -> [AXUIElement] {
        var value: CFTypeRef?
        guard
            AXUIElementCopyAttributeValue(element, attribute as CFString, &value) == .success,
            let values = value as? [AXUIElement]
        else { return [] }
        return values
    }

    private func stringAttribute(_ element: AXUIElement, _ attribute: String) -> String? {
        var value: CFTypeRef?
        guard AXUIElementCopyAttributeValue(element, attribute as CFString, &value) == .success else {
            return nil
        }
        return value as? String
    }

    private func booleanAttribute(_ element: AXUIElement, _ attribute: String) -> Bool? {
        var value: CFTypeRef?
        guard AXUIElementCopyAttributeValue(element, attribute as CFString, &value) == .success else {
            return nil
        }
        return value as? Bool
    }

    private func describe(element: AXUIElement) -> String {
        let text = stringAttribute(element, kAXValueAttribute) ?? ""
        var count: CFTypeRef?
        _ = AXUIElementCopyAttributeValue(element, kAXNumberOfCharactersAttribute as CFString, &count)
        let normalized = text.trimmingCharacters(in: .whitespacesAndNewlines)
        let matchesHint = [kAXTitleAttribute, kAXDescriptionAttribute, kAXPlaceholderValueAttribute].contains {
            stringAttribute(element, $0)?.trimmingCharacters(in: .whitespacesAndNewlines) == normalized
        }
        let attributes: [(String, String)] = [
            ("valueChars", String((text as NSString).length)),
            ("normalizedValueChars", String(((AccessibleText.value(of: element) ?? "") as NSString).length)),
            ("selection", String(describing: AccessibleText.selection(of: element))),
            ("numberOfCharacters", (count as? NSNumber)?.stringValue ?? "?"),
            ("valueMatchesHint", String(matchesHint)),
            ("role", stringAttribute(element, kAXRoleAttribute) ?? "?"),
            ("subrole", stringAttribute(element, kAXSubroleAttribute) ?? "-"),
            ("identifier", stringAttribute(element, kAXIdentifierAttribute) ?? "-"),
            ("title", stringAttribute(element, kAXTitleAttribute) ?? "-"),
            ("description", stringAttribute(element, kAXDescriptionAttribute) ?? "-"),
            ("placeholder", stringAttribute(element, kAXPlaceholderValueAttribute) ?? "-"),
            ("focused", booleanAttribute(element, kAXFocusedAttribute).map(String.init) ?? "?"),
            ("enabled", booleanAttribute(element, kAXEnabledAttribute).map(String.init) ?? "?"),
        ]
        return attributes.map { "\($0.0)=\($0.1)" }.joined(separator: " ")
    }

    private func copyOnly(_ text: String) {
        NSPasteboard.general.clearContents()
        NSPasteboard.general.setString(text, forType: .string)
    }

    private func postKey(keyCode: CGKeyCode, flags: CGEventFlags) {
        let source = CGEventSource(stateID: .hidSystemState)
        let down = CGEvent(keyboardEventSource: source, virtualKey: keyCode, keyDown: true)
        let up = CGEvent(keyboardEventSource: source, virtualKey: keyCode, keyDown: false)
        down?.flags = flags
        up?.flags = flags
        down?.post(tap: .cghidEventTap)
        up?.post(tap: .cghidEventTap)
    }
}

enum CodexEditorSelectionPolicy {
    static func index(candidateCount: Int, focusedIndices: [Int]) -> Int? {
        guard candidateCount > 0 else { return nil }
        if focusedIndices.count == 1, let index = focusedIndices.first, (0..<candidateCount).contains(index) {
            return index
        }
        return candidateCount == 1 && focusedIndices.isEmpty ? 0 : nil
    }
}

enum CodexFocusPolicy {
    static func isVerified(candidateCount: Int, focusMatches: Bool, candidateFocused: Bool, targetIsFrontmost: Bool)
        -> Bool
    {
        candidateCount == 1 && targetIsFrontmost && (focusMatches || candidateFocused)
    }
}

enum PasteboardRestorePolicy {
    static func shouldRestore(expectedChangeCount: Int, currentChangeCount: Int) -> Bool {
        expectedChangeCount == currentChangeCount
    }
}

struct PasteboardSnapshot {
    let items: [[NSPasteboard.PasteboardType: Data]]

    static func capture() -> PasteboardSnapshot {
        let copied = (NSPasteboard.general.pasteboardItems ?? []).map { item in
            Dictionary(
                uniqueKeysWithValues: item.types.compactMap { type in
                    item.data(forType: type).map { (type, $0) }
                })
        }
        return PasteboardSnapshot(items: copied)
    }

    func restore(ifUnchangedSince expectedChangeCount: Int) {
        let pasteboard = NSPasteboard.general
        guard
            PasteboardRestorePolicy.shouldRestore(
                expectedChangeCount: expectedChangeCount,
                currentChangeCount: pasteboard.changeCount
            )
        else { return }
        pasteboard.clearContents()
        let restored: [NSPasteboardItem] = items.map { values in
            let item = NSPasteboardItem()
            for (type, data) in values {
                item.setData(data, forType: type)
            }
            return item
        }
        if !restored.isEmpty { pasteboard.writeObjects(restored) }
    }
}
