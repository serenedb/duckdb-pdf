////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2026 SereneDB GmbH, Berlin, Germany
///
/// Licensed under the Apache License, Version 2.0 (the "License");
/// you may not use this file except in compliance with the License.
/// You may obtain a copy of the License at
///
///     http://www.apache.org/licenses/LICENSE-2.0
///
/// Unless required by applicable law or agreed to in writing, software
/// distributed under the License is distributed on an "AS IS" BASIS,
/// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
/// See the License for the specific language governing permissions and
/// limitations under the License.
///
/// Copyright holder is SereneDB GmbH, Berlin, Germany
////////////////////////////////////////////////////////////////////////////////

#include "podofo_document.hpp"

#include <absl/container/flat_hash_set.h>
#include <duckdb/common/exception.hpp>
#include <duckdb/common/file_system.hpp>
#include <duckdb/common/multi_file/multi_file_reader.hpp>
#include <duckdb/common/types/data_chunk.hpp>
#include <duckdb/common/vector/flat_vector.hpp>
#include <duckdb/common/vector/string_vector.hpp>
#include <duckdb/function/table_function.hpp>
#include <podofo/main/PdfArray.h>
#include <podofo/main/PdfCatalog.h>
#include <podofo/main/PdfDictionary.h>
#include <podofo/main/PdfError.h>
#include <podofo/main/PdfInfo.h>
#include <podofo/main/PdfNameTrees.h>
#include <podofo/main/PdfObjectStream.h>
#include <podofo/main/PdfPageCollection.h>

#include <algorithm>
#include <limits>
#include <vector>

namespace duckdb::pdf {

std::unique_ptr<PoDoFo::PdfMemDocument> OpenDocument(ClientContext &context, const OpenFileInfo &path,
                                                     std::string_view password, std::string &bytes,
                                                     std::string_view function) {
	auto &fs = FileSystem::GetFileSystem(context);
	auto handle = fs.OpenFile(path, FileFlags::FILE_FLAGS_READ);
	const auto size = fs.GetFileSize(*handle);
	if (size <= 0 || !handle->CanSeek()) {
		throw InvalidInputException("%s: expected a nonempty seekable file: %s", function, path.path);
	}
	bytes.resize(size);
	int64_t offset = 0;
	while (offset < size) {
		context.InterruptCheck();
		const auto count = fs.Read(*handle, bytes.data() + offset, std::min<int64_t>(size - offset, 1024 * 1024));
		if (count <= 0) {
			throw IOException("%s: unexpected end of file: %s", function, path.path);
		}
		offset += count;
	}
	auto document = std::make_unique<PoDoFo::PdfMemDocument>();
	document->LoadFromBuffer(PoDoFo::bufferview {bytes.data(), bytes.size()}, password);
	return document;
}

namespace {

enum class InspectKind : uint8_t { Metadata, Outline, Attachments };

struct InspectBindData final : TableFunctionData {
	vector<OpenFileInfo> files;
	std::string password;
	std::string function;
	InspectKind kind = InspectKind::Metadata;
	bool ignore_errors = false;
};

struct InspectNode {
	const PoDoFo::PdfObject *object;
	int32_t depth;
};

struct InspectState final : GlobalTableFunctionState {
	size_t file = 0;
	std::string bytes;
	std::unique_ptr<PoDoFo::PdfMemDocument> document;
	std::vector<InspectNode> pending;
	absl::flat_hash_set<const PoDoFo::PdfObject *> visited;
	const PoDoFo::PdfArray *names = nullptr;
	uint32_t name = 0;
	int32_t ordinal = 0;
};

void SetString(Vector &out, size_t row, std::string_view value) {
	FlatVector::GetDataMutable<string_t>(out)[row] = StringVector::AddString(out, value);
}

std::string_view StringField(const PoDoFo::PdfDictionary &dictionary, std::string_view key) {
	const auto object = dictionary.FindKey(key);
	return object ? object->GetString().GetString() : std::string_view {};
}

unique_ptr<FunctionData> InspectBind(ClientContext &context, TableFunctionBindInput &input, vector<LogicalType> &types,
                                     vector<Identifier> &names) {
	auto bind = make_uniq<InspectBindData>();
	const auto function = input.table_function.GetName();
	bind->function = function.GetIdentifierName();
	if (function == "pdf_outline") {
		bind->kind = InspectKind::Outline;
		names = {"file", "ord", "depth", "title", "page"};
		types = {LogicalType::VARCHAR, LogicalType::INTEGER, LogicalType::INTEGER, LogicalType::VARCHAR,
		         LogicalType::INTEGER};
	} else if (function == "pdf_attachments") {
		bind->kind = InspectKind::Attachments;
		names = {"file", "name", "description", "size", "mime_type", "data"};
		types = {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR,
		         LogicalType::BIGINT,  LogicalType::VARCHAR, LogicalType::BLOB};
	} else {
		names = {"filename", "title",    "author", "subject",     "keywords",
		         "creator",  "producer", "pages",  "pdf_version", "encrypted"};
		types = {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR,
		         LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::INTEGER,
		         LogicalType::VARCHAR, LogicalType::BOOLEAN};
	}
	for (const auto &[key, value] : input.named_parameters) {
		if (value.IsNull()) {
			throw InvalidInputException("PDF: options must not be NULL");
		}
		if (key == "password") {
			bind->password = StringValue::Get(value);
		} else if (key == "ignore_errors") {
			bind->ignore_errors = value.GetValue<bool>();
		}
	}
	bind->files =
	    MultiFileReader::Create(input.table_function)->CreateFileList(context, input.inputs[0])->GetAllFiles();
	return bind;
}

unique_ptr<GlobalTableFunctionState> InspectInit(ClientContext &, TableFunctionInitInput &) {
	return make_uniq<InspectState>();
}

void AdvanceFile(InspectState &state) {
	state.pending.clear();
	state.visited.clear();
	state.names = nullptr;
	state.name = 0;
	state.ordinal = 0;
	state.document.reset();
	state.bytes.clear();
	++state.file;
}

void CheckNode(ClientContext &context, InspectState &state, const PoDoFo::PdfObject *object) {
	context.InterruptCheck();
	if (!state.visited.insert(object).second || state.visited.size() > 100000) {
		throw InvalidInputException("PDF: cyclic or oversized inspection tree");
	}
}

void MetadataRow(InspectState &state, DataChunk &out, size_t row) {
	const auto info = state.document->GetInfo();
	size_t column = 1;
	for (auto key : {"Title", "Author", "Subject", "Keywords", "Creator", "Producer"}) {
		SetString(out.data[column++], row, info ? StringField(info->GetDictionary(), key) : std::string_view {});
	}
	const auto pages = state.document->GetPages().GetCount();
	if (pages > std::numeric_limits<int32_t>::max()) {
		throw InvalidInputException("PDF: page count exceeds INTEGER range");
	}
	FlatVector::GetDataMutable<int32_t>(out.data[7])[row] = pages;
	const auto version = state.document->GetMetadata().GetPdfVersion();
	std::string_view text;
	switch (version) {
	case PoDoFo::PdfVersion::V1_0:
		text = "1.0";
		break;
	case PoDoFo::PdfVersion::V1_1:
		text = "1.1";
		break;
	case PoDoFo::PdfVersion::V1_2:
		text = "1.2";
		break;
	case PoDoFo::PdfVersion::V1_3:
		text = "1.3";
		break;
	case PoDoFo::PdfVersion::V1_4:
		text = "1.4";
		break;
	case PoDoFo::PdfVersion::V1_5:
		text = "1.5";
		break;
	case PoDoFo::PdfVersion::V1_6:
		text = "1.6";
		break;
	case PoDoFo::PdfVersion::V1_7:
		text = "1.7";
		break;
	case PoDoFo::PdfVersion::V2_0:
		text = "2.0";
		break;
	default:
		throw InvalidInputException("PDF: unknown version");
	}
	SetString(out.data[8], row, text);
	FlatVector::GetDataMutable<bool>(out.data[9])[row] = state.document->IsEncrypted();
}

const PoDoFo::PdfObject *ResolveDestination(InspectState &state, const PoDoFo::PdfObject *destination) {
	if (!destination) {
		return nullptr;
	}
	if (destination->IsString() || destination->IsName()) {
		const auto name =
		    destination->IsString() ? destination->GetString().GetString() : destination->GetName().GetString();
		const auto names = state.document->GetNames();
		destination = names ? names->GetValue(PoDoFo::PdfKnownNameTree::Dests, name) : nullptr;
		if (!destination) {
			const auto dests = state.document->GetCatalog().GetDictionary().FindKey("Dests");
			destination = dests ? dests->GetDictionary().FindKey(name) : nullptr;
		}
	}
	if (destination && destination->IsDictionary()) {
		destination = destination->GetDictionary().FindKey("D");
	}
	return destination;
}

void OutlineRow(ClientContext &context, InspectState &state, DataChunk &out, size_t row) {
	const auto node = state.pending.back();
	state.pending.pop_back();
	CheckNode(context, state, node.object);
	const auto &dictionary = node.object->GetDictionary();
	if (const auto next = dictionary.FindKey("Next")) {
		state.pending.push_back({next, node.depth});
	}
	if (const auto child = dictionary.FindKey("First")) {
		state.pending.push_back({child, node.depth + 1});
	}
	FlatVector::GetDataMutable<int32_t>(out.data[1])[row] = ++state.ordinal;
	FlatVector::GetDataMutable<int32_t>(out.data[2])[row] = node.depth;
	SetString(out.data[3], row, StringField(dictionary, "Title"));
	auto destination = dictionary.FindKey("Dest");
	if (!destination) {
		if (const auto action = dictionary.FindKey("A")) {
			const auto subtype = action->GetDictionary().FindKey("S");
			if (subtype && subtype->GetName() == "GoTo") {
				destination = action->GetDictionary().FindKey("D");
			}
		}
	}
	const auto target = ResolveDestination(state, destination);
	FlatVector::SetNull(out.data[4], row, true);
	if (target && target->IsArray() && target->GetArray().GetSize() > 0) {
		const auto page_object = target->GetArray().FindAt(0);
		if (page_object) {
			const PoDoFo::PdfPage *page = nullptr;
			if (state.document->GetPages().TryGetPage(page_object->GetIndirectReference(), page)) {
				FlatVector::SetNull(out.data[4], row, false);
				FlatVector::GetDataMutable<int32_t>(out.data[4])[row] = page->GetPageNumber();
			}
		}
	}
}

const PoDoFo::PdfObject *NextAttachment(ClientContext &context, InspectState &state) {
	while (true) {
		context.InterruptCheck();
		if (state.names && state.name < state.names->GetSize()) {
			const auto object = state.names->FindAt(state.name + 1);
			state.name += 2;
			if (!object) {
				throw InvalidInputException("PDF: invalid attachment reference");
			}
			return object;
		}
		if (state.pending.empty()) {
			return nullptr;
		}
		const auto node = state.pending.back().object;
		state.pending.pop_back();
		CheckNode(context, state, node);
		const auto &dictionary = node->GetDictionary();
		if (const auto kids = dictionary.FindKey("Kids")) {
			const auto &array = kids->GetArray();
			if (array.GetSize() > 100000 - state.pending.size()) {
				throw InvalidInputException("PDF: oversized attachment tree");
			}
			for (size_t i = array.GetSize(); i > 0; --i) {
				state.pending.push_back({&array.MustFindAt(i - 1), 0});
			}
		}
		const auto names = dictionary.FindKey("Names");
		state.names = names ? &names->GetArray() : nullptr;
		state.name = 0;
		if (state.names && state.names->GetSize() % 2) {
			throw InvalidInputException("PDF: odd attachment name array");
		}
	}
}

void AttachmentRow(const PoDoFo::PdfObject &object, DataChunk &out, size_t row) {
	const auto &dictionary = object.GetDictionary();
	const auto unicode = dictionary.FindKey("UF");
	SetString(out.data[1], row, unicode ? unicode->GetString().GetString() : StringField(dictionary, "F"));
	SetString(out.data[2], row, StringField(dictionary, "Desc"));
	const auto &embedded = dictionary.MustFindKey("EF").GetDictionary();
	const auto unicode_stream = embedded.FindKey("UF");
	const auto &stream = unicode_stream ? *unicode_stream : embedded.MustFindKey("F");
	const auto content = stream.GetStream();
	if (!content) {
		throw InvalidInputException("PDF: attachment has no embedded stream");
	}
	const auto bytes = content->GetCopy();
	FlatVector::GetDataMutable<int64_t>(out.data[3])[row] = bytes.size();
	const auto subtype = stream.GetDictionary().FindKey("Subtype");
	SetString(out.data[4], row, subtype ? subtype->GetName().GetString() : std::string_view {});
	FlatVector::GetDataMutable<string_t>(out.data[5])[row] =
	    StringVector::AddStringOrBlob(out.data[5], bytes.data(), bytes.size());
}

void InspectScan(ClientContext &context, TableFunctionInput &input, DataChunk &out) {
	const auto &bind = input.bind_data->Cast<InspectBindData>();
	auto &state = input.global_state->Cast<InspectState>();
	size_t count = 0;
	try {
		while (count < STANDARD_VECTOR_SIZE && state.file < bind.files.size()) {
			context.InterruptCheck();
			if (!state.document) {
				try {
					state.document =
					    OpenDocument(context, bind.files[state.file], bind.password, state.bytes, bind.function);
				} catch (const PoDoFo::PdfError &) {
					if (!bind.ignore_errors) {
						throw;
					}
					AdvanceFile(state);
					continue;
				} catch (const IOException &) {
					if (!bind.ignore_errors) {
						throw;
					}
					AdvanceFile(state);
					continue;
				} catch (const InvalidInputException &) {
					if (!bind.ignore_errors) {
						throw;
					}
					AdvanceFile(state);
					continue;
				}
				const auto &catalog = state.document->GetCatalog().GetDictionary();
				if (bind.kind == InspectKind::Outline) {
					const auto outlines = catalog.FindKey("Outlines");
					if (outlines) {
						if (const auto first = outlines->GetDictionary().FindKey("First")) {
							state.pending.push_back({first, 1});
						}
					}
				} else if (bind.kind == InspectKind::Attachments) {
					const auto names = catalog.FindKey("Names");
					if (names) {
						if (const auto files = names->GetDictionary().FindKey("EmbeddedFiles")) {
							state.pending.push_back({files, 0});
						}
					}
				}
			}
			SetString(out.data[0], count, bind.files[state.file].path);
			if (bind.kind == InspectKind::Metadata) {
				MetadataRow(state, out, count++);
				AdvanceFile(state);
			} else if (bind.kind == InspectKind::Outline) {
				if (state.pending.empty()) {
					AdvanceFile(state);
					continue;
				}
				OutlineRow(context, state, out, count++);
			} else {
				const auto attachment = NextAttachment(context, state);
				if (!attachment) {
					AdvanceFile(state);
					continue;
				}
				AttachmentRow(*attachment, out, count++);
			}
		}
	} catch (const std::length_error &error) {
		throw InvalidInputException("could not inspect PDF: %s", error.what());
	} catch (const PoDoFo::PdfError &error) {
		if (error.GetCode() == PoDoFo::PdfErrorCode::InvalidPassword) {
			throw InvalidInputException("PDF is encrypted; supply the correct password");
		}
		throw InvalidInputException("could not inspect PDF: %s", error.GetName());
	}
	out.SetChildCardinality(count);
}

} // namespace

void LoadInspectionFunctions(ExtensionLoader &loader) {
	for (auto name : {"read_pdf_meta", "pdf_outline", "pdf_attachments"}) {
		FunctionSignature signature;
		signature.AddPositionalOnly("path", LogicalType::VARCHAR)
		    .WithTypedKwargs("options", [name](TypedKwargs &options) {
			    options.Add("password", LogicalType::VARCHAR);
			    if (std::string_view(name) != "pdf_attachments") {
				    options.Add("ignore_errors", LogicalType::BOOLEAN);
			    }
		    });
		loader.RegisterFunction(TableFunction {name, std::move(signature), InspectScan, InspectBind, InspectInit});
	}
}

} // namespace duckdb::pdf
