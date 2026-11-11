import unittest

from tools.common import traditional_to_simplified


class TraditionalToSimplifiedTest(unittest.TestCase):
    def test_converts_traditional_characters(self):
        self.assertEqual(traditional_to_simplified("頭髮"), "头发")
        self.assertEqual(traditional_to_simplified("憂鬱的臺灣烏龜"), "忧郁的台湾乌龟")
        self.assertEqual(traditional_to_simplified("漢字"), "汉字")

    def test_mixed_text(self):
        self.assertEqual(
            traditional_to_simplified("我愛臺灣，也愛北京"), "我爱台湾，也爱北京"
        )

    def test_returns_original_when_no_traditional(self):
        text = "简体中文 abc 123"
        self.assertIs(traditional_to_simplified(text), text)

    def test_empty(self):
        text = ""
        self.assertIs(traditional_to_simplified(text), text)

    def test_non_string_is_unchanged(self):
        self.assertIsNone(traditional_to_simplified(None))
        value = 123
        self.assertIs(traditional_to_simplified(value), value)


if __name__ == "__main__":
    unittest.main()
