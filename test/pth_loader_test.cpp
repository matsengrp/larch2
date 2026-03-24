#include <larch/pth_loader.hpp>
#include <larch/yaml_reader.hpp>

#include <cassert>
#include <cmath>
#include <print>

using namespace larch;

// ---- ZIP reader tests ----

void test_zip_reader_s5f() {
  zip_reader zip{"data/bcr/s5f-libtorch.pth"};

  // Should have entries with a common prefix.
  auto pfx = zip.prefix();
  assert(!pfx.empty());
  assert(pfx == "s5f-libtorch/");

  // Should contain data.pkl and data files.
  assert(zip.contains_suffix("data.pkl"));
  assert(zip.contains_suffix("byteorder"));

  auto pkl = zip.get_by_suffix("data.pkl");
  assert(pkl.size() == 277);

  auto bo = zip.get_by_suffix("byteorder");
  std::string_view bo_str{reinterpret_cast<const char*>(bo.data()), bo.size()};
  assert(bo_str == "little");

  // data/0: r_kmer_embedding.weight storage, 1025 floats = 4100 bytes
  auto d0 = zip.get("s5f-libtorch/data/0");
  assert(d0.size() == 4100);

  // data/1: s_kmer_embedding.weight storage, 1025*4=4100 floats = 16400 bytes
  auto d1 = zip.get("s5f-libtorch/data/1");
  assert(d1.size() == 16400);

  std::println("  zip_reader s5f: OK");
}

void test_zip_reader_cnn() {
  zip_reader zip{"data/bcr/ThriftyHumV0.2-45-libtorch.pth"};

  assert(zip.prefix() == "ThriftyHumV0.2-45-libtorch/");
  assert(zip.contains_suffix("data.pkl"));

  auto pkl = zip.get_by_suffix("data.pkl");
  assert(pkl.size() == 1220);

  // Verify a few data file sizes.
  // data/0: kmer_embedding.weight, 65*7=455 floats = 1820 bytes
  auto d0 = zip.get("ThriftyHumV0.2-45-libtorch/data/0");
  assert(d0.size() == 1820);

  // data/1: conv.weight, 16*7*9=1008 floats = 4032 bytes
  auto d1 = zip.get("ThriftyHumV0.2-45-libtorch/data/1");
  assert(d1.size() == 4032);

  std::println("  zip_reader cnn: OK");
}

void test_zip_reader_entries() {
  zip_reader zip{"data/bcr/s5f-libtorch.pth"};
  auto entries = zip.entries();
  // Should have: data.pkl, .format_version, .storage_alignment, byteorder,
  //              data/0, data/1, version, .data/serialization_id = 8
  assert(entries.size() == 8);
  std::println("  zip_reader entries: OK");
}

// ---- Pickle parser tests ----

void test_pickle_s5f() {
  zip_reader zip{"data/bcr/s5f-libtorch.pth"};
  auto pkl = zip.get_by_suffix("data.pkl");
  auto info = parse_state_dict(pkl);

  assert(info.tensors.size() == 2);

  // r_kmer_embedding.weight: shape [1025, 1], storage "0"
  assert(info.tensors[0].name == "r_kmer_embedding.weight");
  assert(info.tensors[0].storage_key == "0");
  assert(info.tensors[0].shape.size() == 2);
  assert(info.tensors[0].shape[0] == 1025);
  assert(info.tensors[0].shape[1] == 1);
  assert(info.tensors[0].num_elements == 1025);

  // s_kmer_embedding.weight: shape [1025, 4], storage "1"
  assert(info.tensors[1].name == "s_kmer_embedding.weight");
  assert(info.tensors[1].storage_key == "1");
  assert(info.tensors[1].shape.size() == 2);
  assert(info.tensors[1].shape[0] == 1025);
  assert(info.tensors[1].shape[1] == 4);
  assert(info.tensors[1].num_elements == 4100);

  std::println("  pickle s5f: OK");
}

void test_pickle_cnn() {
  zip_reader zip{"data/bcr/ThriftyHumV0.2-45-libtorch.pth"};
  auto pkl = zip.get_by_suffix("data.pkl");
  auto info = parse_state_dict(pkl);

  // CNN model has 15 parameters:
  //   5 base (kmer_embedding, conv.w, conv.b, linear.w, linear.b)
  //   5 R-branch (r_kmer_embedding, r_conv.w, r_conv.b, r_linear.w, r_linear.b)
  //   5 S-branch (s_kmer_embedding, s_conv.w, s_conv.b, s_linear.w, s_linear.b)
  assert(info.tensors.size() == 15);

  // Spot-check a few parameters.
  auto find = [&](std::string_view name) -> tensor_info const* {
    for (auto& t : info.tensors) {
      if (t.name == name) return &t;
    }
    return nullptr;
  };

  auto* kmer_emb = find("kmer_embedding.weight");
  assert(kmer_emb);
  assert((kmer_emb->shape == std::vector<int64_t>{65, 7}));

  auto* conv_w = find("conv.weight");
  assert(conv_w);
  assert(conv_w->shape == (std::vector<int64_t>{16, 7, 9}));

  auto* r_lin_w = find("r_linear.weight");
  assert(r_lin_w);
  assert(r_lin_w->shape == (std::vector<int64_t>{1, 16}));

  auto* s_lin_w = find("s_linear.weight");
  assert(s_lin_w);
  assert(s_lin_w->shape == (std::vector<int64_t>{4, 16}));

  auto* s_lin_b = find("s_linear.bias");
  assert(s_lin_b);
  assert(s_lin_b->shape == std::vector<int64_t>{4});

  std::println("  pickle cnn: OK");
}

// ---- YAML reader tests ----

void test_yaml_s5f() {
  auto doc = read_yaml("data/bcr/s5f.yml");

  assert(doc.at("model_class").as_string() == "RSFivemerModel");
  assert(doc.at("encoder_class").as_string() == "KmerSequenceEncoder");
  assert(doc.at("serialization_version").as_int() == 0);

  assert(doc.at("encoder_parameters").is_map());
  assert(doc.at("encoder_parameters").map.at("kmer_length").as_int() == 5);
  assert(doc.at("encoder_parameters").map.at("site_count").as_int() == 500);

  assert(doc.at("model_hyperparameters").is_map());
  assert(doc.at("model_hyperparameters").map.at("kmer_length").as_int() == 5);

  std::println("  yaml s5f: OK");
}

void test_yaml_cnn() {
  auto doc = read_yaml("data/bcr/ThriftyHumV0.2-45.yml");

  assert(doc.at("model_class").as_string() == "IndepRSCNNModel");
  assert(doc.at("encoder_class").as_string() == "KmerSequenceEncoder");

  auto& enc = doc.at("encoder_parameters").map;
  assert(enc.at("kmer_length").as_int() == 3);
  assert(enc.at("site_count").as_int() == 500);

  auto& hyp = doc.at("model_hyperparameters").map;
  assert(hyp.at("embedding_dim").as_int() == 7);
  assert(hyp.at("filter_count").as_int() == 16);
  assert(hyp.at("kernel_size").as_int() == 9);
  assert(hyp.at("kmer_length").as_int() == 3);
  assert(std::abs(hyp.at("dropout_prob").as_float() - 0.2) < 1e-9);

  auto& train = doc.at("training_hyperparameters").map;
  assert(std::abs(train.at("learning_rate").as_float() - 0.001) < 1e-9);
  assert(std::abs(train.at("min_learning_rate").as_float() - 1e-6) < 1e-15);
  assert(std::abs(train.at("weight_decay").as_float() - 1e-6) < 1e-15);

  std::println("  yaml cnn: OK");
}

// ---- PTH loader tests ----

void test_pth_s5f() {
  auto pth = load_pth("data/bcr/s5f-libtorch.pth");

  assert(pth.tensors.size() == 2);

  auto& r = pth.get("r_kmer_embedding.weight");
  assert(r.shape == (std::vector<int64_t>{1025, 1}));
  assert(r.data.size() == 1025);

  auto& s = pth.get("s_kmer_embedding.weight");
  assert(s.shape == (std::vector<int64_t>{1025, 4}));
  assert(s.data.size() == 4100);

  // Verify the data is readable and contains finite values.
  for (float v : r.data) assert(std::isfinite(v));
  for (float v : s.data) assert(std::isfinite(v));

  std::println("  pth s5f: OK");
}

void test_pth_cnn() {
  auto pth = load_pth("data/bcr/ThriftyHumV0.2-45-libtorch.pth");

  assert(pth.tensors.size() == 15);

  // Verify R-branch conv weight.
  auto& r_conv = pth.get("r_conv.weight");
  assert(r_conv.shape == (std::vector<int64_t>{16, 7, 9}));
  assert(r_conv.data.size() == 16 * 7 * 9);
  for (float v : r_conv.data) assert(std::isfinite(v));

  // Verify S-branch linear weight.
  auto& s_lin = pth.get("s_linear.weight");
  assert(s_lin.shape == (std::vector<int64_t>{4, 16}));
  assert(s_lin.data.size() == 64);

  // Verify S-branch linear bias.
  auto& s_bias = pth.get("s_linear.bias");
  assert(s_bias.shape == std::vector<int64_t>{4});
  assert(s_bias.data.size() == 4);

  std::println("  pth cnn: OK");
}

void test_pth_find_missing() {
  auto pth = load_pth("data/bcr/s5f-libtorch.pth");
  assert(pth.find("nonexistent") == nullptr);
  std::println("  pth find missing: OK");
}

int main() {
  std::println("=== ZIP reader tests ===");
  test_zip_reader_s5f();
  test_zip_reader_cnn();
  test_zip_reader_entries();

  std::println("=== Pickle parser tests ===");
  test_pickle_s5f();
  test_pickle_cnn();

  std::println("=== YAML reader tests ===");
  test_yaml_s5f();
  test_yaml_cnn();

  std::println("=== PTH loader tests ===");
  test_pth_s5f();
  test_pth_cnn();
  test_pth_find_missing();

  std::println("All pth_loader tests passed");
}
