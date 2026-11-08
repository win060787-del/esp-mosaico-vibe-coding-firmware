import AppKit
import ApplicationServices
import Foundation

enum AccessibleText {
    static func normalizedValue(
        _ raw: String, title: String?, description: String?, placeholder: String?, selection: NSRange?
    ) -> String {
        let text = raw.trimmingCharacters(in: .whitespacesAndNewlines)
        guard selection == NSRange(location: 0, length: 0), !text.isEmpty else { return raw }
        let trim: (String?) -> String? = { $0?.trimmingCharacters(in: .whitespacesAndNewlines) }
        if text == trim(placeholder) || (text == trim(title) && text == trim(description)) { return "" }
        return raw
    }
    static func string(_ name: String, of element: AXUIElement) -> String? {
        var value: CFTypeRef?
        guard AXUIElementCopyAttributeValue(element, name as CFString, &value) == .success else { return nil }
        return value as? String
    }
    static func value(of element: AXUIElement) -> String? {
        guard let raw = string(kAXValueAttribute, of: element) else { return nil }
        return normalizedValue(
            raw, title: string(kAXTitleAttribute, of: element),
            description: string(kAXDescriptionAttribute, of: element),
            placeholder: string(kAXPlaceholderValueAttribute, of: element), selection: selection(of: element))
    }
    static func selection(of element: AXUIElement) -> NSRange? {
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
    static func select(_ range: NSRange, in element: AXUIElement) -> Bool {
        var range = CFRange(location: range.location, length: range.length)
        guard let value = AXValueCreate(.cfRange, &range) else { return false }
        return AXUIElementSetAttributeValue(element, kAXSelectedTextRangeAttribute as CFString, value) == .success
    }
    static func focused(_ element: AXUIElement, processID: pid_t) -> Bool {
        guard NSWorkspace.shared.frontmostApplication?.processIdentifier == processID else { return false }
        var value: CFTypeRef?
        let app = AXUIElementCreateApplication(processID)
        if AXUIElementCopyAttributeValue(app, kAXFocusedUIElementAttribute as CFString, &value) == .success,
            let value, CFEqual(value, element)
        {
            return true
        }
        return AXUIElementCopyAttributeValue(element, kAXFocusedAttribute as CFString, &value) == .success
            && value as? Bool == true
    }
}
