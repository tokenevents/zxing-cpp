#include "BarcodeFormat.h"

// Reader
#include "DecodeHints.h"
#include "GenericLuminanceSource.h"
#include "HybridBinarizer.h"
#include "MultiFormatReader.h"
#include "Result.h"

// Writer
#include "BitMatrix.h"
#include "MultiFormatWriter.h"
#include "TextUtfEncoding.h"

#include <pybind11/pybind11.h>
#include <pybind11/numpy.h>
#include <pybind11/stl.h>
#include <memory>
#include <vector>

using namespace ZXing;
namespace py = pybind11;

// Numpy array wrapper class for images (either BGR or GRAYSCALE)
using Image = py::array_t<uint8_t, py::array::c_style>;

Result read_barcode(const Image& image, BarcodeFormats formats, bool fastMode, bool tryRotate, bool hybridBinarizer)
{
	DecodeHints hints;
	hints.setTryHarder(!fastMode);
	hints.setTryRotate(tryRotate);
	hints.setPossibleFormats(formats);
	MultiFormatReader reader(hints);
	const auto height = static_cast<int>(image.shape(0));
	const auto width = static_cast<int>(image.shape(1));
	const auto bytes = image.data();
	std::shared_ptr<LuminanceSource> source;

	if (image.ndim() == 2) {
		// Grayscale image
		source = std::make_shared<GenericLuminanceSource>(width, height, bytes, width);
	} else {
		// BGR image
		const auto channels = image.shape(2);
		source = std::make_shared<GenericLuminanceSource>(width, height, bytes, width * channels, channels, 2, 1, 0);
	}

	if (hybridBinarizer) {
		return reader.read(HybridBinarizer(source));
	} else {
		return reader.read(GlobalHistogramBinarizer(source));
	}
}

Image write_barcode(BarcodeFormat format, std::string text, int width, int height, int margin, int eccLevel)
{
	auto writer = MultiFormatWriter(format).setMargin(margin).setEccLevel(eccLevel);
	auto bitmap = writer.encode(TextUtfEncoding::FromUtf8(text), width, height);

	auto result = Image({bitmap.width(), bitmap.height()});
	auto r = result.mutable_unchecked<2>();
	for (ssize_t y = 0; y < r.shape(0); y++)
		for (ssize_t x = 0; x < r.shape(1); x++)
			r(y, x) = bitmap.get(x, y) ? 0 : 255;
	return result;
}

PYBIND11_MODULE(zxing, m)
{
	m.doc() = "python bindings for zxing-cpp";
	py::enum_<BarcodeFormat>(m, "BarcodeFormat")
		.value("AZTEC", BarcodeFormat::AZTEC)
		.value("CODABAR", BarcodeFormat::CODABAR)
		.value("CODE_39", BarcodeFormat::CODE_39)
		.value("CODE_93", BarcodeFormat::CODE_93)
		.value("CODE_128", BarcodeFormat::CODE_128)
		.value("DATA_MATRIX", BarcodeFormat::DATA_MATRIX)
		.value("EAN_8", BarcodeFormat::EAN_8)
		.value("EAN_13", BarcodeFormat::EAN_13)
		.value("ITF", BarcodeFormat::ITF)
		.value("MAXICODE", BarcodeFormat::MAXICODE)
		.value("PDF_417", BarcodeFormat::PDF_417)
		.value("QR_CODE", BarcodeFormat::QR_CODE)
		.value("RSS_14", BarcodeFormat::RSS_14)
		.value("RSS_EXPANDED", BarcodeFormat::RSS_EXPANDED)
		.value("UPC_A", BarcodeFormat::UPC_A)
		.value("UPC_E", BarcodeFormat::UPC_E)
		.value("UPC_EAN_EXTENSION", BarcodeFormat::UPC_EAN_EXTENSION)
		.value("FORMAT_COUNT", BarcodeFormat::FORMAT_COUNT)
		.export_values();
	py::class_<ResultPoint>(m, "ResultPoint")
		.def_property_readonly("x", &ResultPoint::x)
		.def_property_readonly("y", &ResultPoint::y);
	py::class_<Result>(m, "Result")
		.def_property_readonly("valid", &Result::isValid)
		.def_property_readonly("text", &Result::text)
		.def_property_readonly("format", &Result::format)
		.def_property_readonly("points", &Result::resultPoints);
	m.def("read_barcode", &read_barcode, "Read (decode) a barcode from a numpy BGR or grayscale image array",
		py::arg("image"),
		py::arg("format") = BarcodeFormats{},
		py::arg("fastMode") = false,
		py::arg("tryRotate") = true,
		py::arg("hybridBinarizer") = true
	);
	m.def("write_barcode", &write_barcode, "Write (encode) a text into a barcode and return numpy image array",
		py::arg("format"),
		py::arg("text"),
		py::arg("width") = 0,
		py::arg("height") = 0,
		py::arg("margin") = -1,
		py::arg("eccLevel") = -1
	);
}
