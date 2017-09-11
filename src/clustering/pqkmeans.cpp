#include "./pqkmeans.h"

namespace pqkmeans {


PQKmeans::PQKmeans(std::vector<std::vector<std::vector<float> > > codewords, int K, int itr)
    : codewords_(codewords), K_(K), itr_(itr)
{
    assert(!codewords.empty() && !codewords[0].empty() && !codewords[0][0].empty());
    M_ = codewords.size(); // The number of subspace
    std::size_t Ks = codewords[0].size();  // The number of codewords for each subspace
    assert(Ks == codewords[0][0].size());

    // Current implementaion only supports the case when Ks < 256
    // so that each element of the code must be unsigned char
    assert(Ks <= 256);

    // Compute distance-matrices among codewords
    distance_matrices_among_codewords_.resize(
                M_, std::vector<std::vector<float>>(Ks, std::vector<float>(Ks, 0)));

    for (std::size_t m = 0; m < M_; ++m) {
        for (std::size_t k1 = 0; k1 < Ks; ++k1) {
            for (std::size_t k2 = 0; k2 < Ks; ++k2) {
                distance_matrices_among_codewords_[m][k1][k2] =
                        L2SquaredDistance(codewords[m][k1], codewords[m][k2]);
            }
        }
    }
}

int PQKmeans::predict_one(const std::vector<float> &pyvector)
{
    return 0;
}

void PQKmeans::fit(const std::vector<std::vector<unsigned char> > &pydata) {
    assert(K_ < (int) pydata.size());
    std::size_t N = pydata.size();

    // Refresh
    centroids.clear();
    centroids.resize((std::size_t) K_, std::vector<unsigned char>(M_));
    assignments.clear();
    assignments.resize(pydata.size());
    assignments.shrink_to_fit(); // If the previous fit mallocs a long assignment array, shrink it.

    // Prepare data temporal buffer
    std::vector<std::vector<unsigned char>> centroids_new, centroids_old;

    // (1) Initialization
    // [todo] Currently, only random pick is supported
    InitializeCentroidsByRandomPicking(pydata, K_, &centroids_new);

    // selected_indices_foreach_centroid[k] has indices, where
    // each pydata[id] is assigned to k-th centroid.
    std::vector<std::vector<std::size_t>> selected_indices_foreach_centroid(K_);
    for (auto &selected_indices : selected_indices_foreach_centroid) {
        selected_indices.reserve( N / K_); // roughly allocate
    }

    std::vector<double> errors(N, 0);

    for (int itr = 0; itr < itr_; ++itr) {
        std::cout << "Iteration start: " << itr << " / " << itr_ << std::endl;
        auto start = std::chrono::system_clock::now(); // ---- timer start ---

        centroids_old = centroids_new;

        // (2) Find NN centroids
        selected_indices_foreach_centroid.clear();
        selected_indices_foreach_centroid.resize(K_);

        double error_sum = 0;

#pragma omp parallel for
        for(std::size_t n = 0; n < N; ++n) {
            std::pair<std::size_t, float> min_k_dist = FindNNLinear(pydata[n], centroids_old);
            assignments[n] = (int) min_k_dist.first;
            errors[n] = min_k_dist.second;
        }
        // (2.5) assignments -> selected_indices_foreach_centroid
        for (std::size_t n = 0; n < N; ++n) {
            int k = assignments[n];
            selected_indices_foreach_centroid[k].push_back(n);
            error_sum += errors[n];
        }

        std::cout << "find_nn finished. Error: " << error_sum / N << std::endl;
        std::cout << "find_nn_time,"
                  << std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now() - start).count()
                  << std::endl;

        // (3) Compute centroids
        if (itr != itr_ - 1) {
            // Usually, centroids would be updated.
            // After the last assignment, centroids should not be updated, so this block is skiped.

            for (int k = 0; k < K_; ++k) {
                if (selected_indices_foreach_centroid[k].empty()) {
                    std::cout << "Caution. No codes are assigned to " << k << "-th centroids." << std::endl;
                    continue;
                }
                centroids_new[k] = ComputeCentroidBySparseVoting(pydata,
                                                                 selected_indices_foreach_centroid[k]);

            }
        }
        std::cout << "find_nn+update_center_time,"
                  << std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now() - start).count()
                  << std::endl;

    }
    centroids = centroids_new;
}

float PQKmeans::SymmetricDistance(const std::vector<unsigned char> &code1,
                                  const std::vector<unsigned char> &code2)
{
    // assert(code1.size() == code2.size());
    // assert(code1.size() == M_);
    float dist = 0;
    for (std::size_t m = 0; m < M_; ++m) {
        dist += distance_matrices_among_codewords_[m][code1[m]][code2[m]];
    }
    return dist;
}

float PQKmeans::L2SquaredDistance(const std::vector<float> &vec1,
                                  const std::vector<float> &vec2)
{
    assert(vec1.size() == vec2.size());
    float dist = 0;
    for (std::size_t i = 0; i < vec1.size(); ++i) {
        dist += (vec1[i] - vec2[i]) * (vec1[i] - vec2[i]);
    }
    return dist;
}

void PQKmeans::InitializeCentroidsByRandomPicking(const std::vector<std::vector<unsigned char> > &codes,
                                                  int K,
                                                  std::vector<std::vector<unsigned char> > *centroids)
{
    assert(centroids != nullptr);
    centroids->clear();
    centroids->resize(K);

    std::vector<int> ids(codes.size());
    std::iota(ids.begin(), ids.end(), 0); // 0, 1, 2, ..., codes.size()-1
    std::mt19937 random_engine(0);
    std::shuffle(ids.begin(), ids.end(), random_engine);
    for (std::size_t k = 0; k < (std::size_t) K; ++k) {
        (*centroids)[k] = codes[ids[k]];
    }
}

std::pair<std::size_t, float> PQKmeans::FindNNLinear(const std::vector<unsigned char> &query,
                                                     const std::vector<std::vector<unsigned char> > &codes)
{
    int min_i = -1;
    float min_dist = FLT_MAX;
    for (std::size_t i = 0, sz = codes.size(); i < sz; ++i) {
        float dist = SymmetricDistance(query, codes[i]);
        if (dist < min_dist) {
            min_i = i;
            min_dist = dist;
        }
    }
    assert(min_i != -1);
    return std::pair<std::size_t, float>((std::size_t) min_i, min_dist);
}

std::vector<unsigned char> PQKmeans::ComputeCentroidBySparseVoting(const std::vector<std::vector<unsigned char> > &codes,
                                                                   const std::vector<std::size_t> &selected_ids)
{
    std::vector<unsigned char> average_code(M_);
    std::size_t Ks = codewords_[0].size();  // The number of codewords for each subspace

    for (std::size_t m = 0; m < M_; ++m) {
        // Scan the assigned codes, then create a freq-histogram
        std::vector<int> frequency_histogram(Ks, 0);
        for (const auto &id : selected_ids) {
            ++frequency_histogram[codes[id][m]];
        }

        // Vote the freq-histo with weighted by ditance matrices
        std::vector<float> vote(Ks, 0);
        for (std::size_t k1 = 0; k1 < Ks; ++k1) {
            int freq = frequency_histogram[k1];
            if (freq == 0) { // not assigned for k1. Skip it.
                continue;
            }
            for (std::size_t k2 = 0; k2 < Ks; ++k2) {
                vote[k2] += (float) freq * distance_matrices_among_codewords_[m][k1][k2];
            }
        }

        // find min
        float min_dist = FLT_MAX;
        int min_ks = -1;
        for (std::size_t ks = 0; ks < Ks; ++ks) {
            if (vote[ks] < min_dist) {
                min_ks = (int) ks;
                min_dist = vote[ks];
            }
        }
        assert(min_ks != -1);
        average_code[m] = (unsigned char) min_ks;
    }
    return average_code;
}



} // namespace pqkmeans