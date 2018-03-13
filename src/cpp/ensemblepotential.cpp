//
// Created by Eric Irrgang on 2/26/18.
//

#include "ensemblepotential.h"

#include <vector>

namespace plugin
{

template<typename T>
void EnsembleResourceHandle::reduce(const std::vector<T>& input, std::vector<T>* output)
{}

template<typename T_I, typename T_O>
void EnsembleResourceHandle::map_reduce(const T_I &iterable,
                                        T_O *output,
                                        void (*function)(double, const PairHist & input,
                                                 PairHist * output)
                                        )
{}

//// From the full set of trajectories in the ensemble, calculate the new smoothed difference between the experimental
//// distribution and sampled distribution.
//void calculate_histogram(std::vector<pair_data> vec_pd,
//                         const char *out_filename,
//                         parameters params,
//                         int ensemble_number) {
//
//
////    if (boost::filesystem::exists(out_filename)) {
////        backup_file(out_filename, ensemble_number - 1);
////    }
////
////    std::ofstream hist_file;
////    hist_file.open(out_filename);
////
////    hist_file << params.bin_width << "," << params.sigma << ",";
////    hist_file << params.min_dist << "," << params.max_dist << "\n";
//
//    // Normalizing coefficient for a Gaussian (normal) distribution.
//    auto norm = 1.0 / sqrt(2.0 * M_PI * pow(params.sigma, 2.0));
//
//    // for each pair?
//    for (auto &pd: vec_pd) {
//        auto &exp_data = pd.exp_distribution;
//        if (exp_data.empty()) {
////            char error[BUFFER_LENGTH];
////            snprintf(error, BUFFER_LENGTH, "Cannot calculate histogram if experimental distribution is empty");
//            throw std::invalid_argument(error);
//        }
//
//        auto &sim_data = pd.sim_dist_data;
//        auto num_bins = exp_data.size();
//        auto num_samples = sim_data.size();
//        // Adjust normalization to average a sum of num_samples distributions.
//        auto sample_norm = (1.0 / num_samples) * norm;
//
//        // for each bin
//        for (int n = 0; n < num_bins; ++n) {
//            double h_ij_n{0}, dif_ij_n{0};
//            // for each distance value in the samples for this simulation, Gaussian blur to this bin.
//            for (auto &sample_dist: sim_data) {
//                h_ij_n += sample_norm * exp(-pow(n * params.bin_width - sample_dist,
//                                                 2.0) / (2 * pow(params.sigma, 2)));
//            }
//            // Record the difference between the smoothed data and the experimental data.
//            dif_ij_n = h_ij_n - exp_data.at(n);
//
//            if (n == 0) hist_file << dif_ij_n;
//            else hist_file << "," << dif_ij_n;
//        }
//        hist_file << "\n";
//    }
//    hist_file.close();
//}

/*!
 * \brief Apply a Gaussian blur when building a density grid for a list of values.
 *
 * Normalize such that the area under each sample is 1.0/num_samples.
 */
class BlurToGrid
{
    public:
        BlurToGrid(double min_dist, double max_dist, double sigma) :
            _min_dist{min_dist},
            _max_dist{max_dist},
            _sigma{sigma}
        {
        };

        void operator() (const std::vector<double>& distances, std::vector<double>* grid)
        {
            const auto nbins = grid->size();
            const double dx{(_max_dist - _min_dist)/nbins};
            const auto num_samples = distances.size();

            const double denominator = 1.0/(2*_sigma*_sigma);
            const double normalization = 1.0/(num_samples*sqrt(2.0*M_PI*_sigma*_sigma));
            // We aren't doing any filtering of values too far away to contribute meaningfully, which
            // is admittedly wasteful for large sigma...
            for (size_t i = 0; i < nbins; ++i)
            {
                double bin_value{0};
                const double bin_x{i*dx};
                for(const auto distance : distances)
                {
                    const double relative_distance{bin_x - distance};
                    const auto numerator = -relative_distance*relative_distance;
                    bin_value += normalization*exp(numerator*denominator);
                }
                grid->at(i) = bin_value;
            }
        };

    private:
        /// Minimum value of bin zero
        const double _min_dist;
        /// Maximum value of bin
        const double _max_dist;
        const double _sigma;
};


EnsembleHarmonic::EnsembleHarmonic() :
    _nbins{0},
    _binWidth{0},
    _min_dist{0},
    _max_dist{0},
    _histogram{nullptr},
    _experimental{nullptr},
    _nsamples{0},
    _current_sample{0},
    _sample_period{0},
    _next_sample_time{_sample_period},
    _distance_samples(_nsamples),
    _nwindows{0},
    _current_window{0},
    _window_update_period{0},
    _next_window_update_time{_window_update_period},
    _windows(),
    _K{0},
    _sigma{0}
{
    // We leave _histogram and _experimental unallocated until we have valid data to put in them, so that
    // (_histogram == nullptr) == invalid histogram.
}

//template<typename TS, typename TX>
//class Discretizer
//{
//    public:
//        Discretizer(TS size, TX minimum, TX maximum):
//            _min{minimum},
//            _max{maximum},
//            _dx{(maximum - minimum)/size}
//        {};
//
//        TS operator()(TX value) const
//        {
//            return static_cast<TS>(floor((value - _min)/_dx));
//        }
//
//    private:
//        const TX _min;
//        const TX _max;
//        const TX _dx;
//};
//// Class template argument deduction not available until C++17, so we use a factory function for static dispatch.
//template<typename TS, typename TX>
//Discretizer<TS, TX> make_discretizer(TS size, TX minimum, TX maximum)
//{
//    return Discretizer<TS, TX>(size, minimum, maximum);
//};

gmx::PotentialPointData EnsembleHarmonic::calculate(gmx::Vector v,
                                                    gmx::Vector v0,
                                                    double t)
{
    auto rdiff = v - v0;
    const auto Rsquared = dot(rdiff, rdiff);
    const auto R = sqrt(Rsquared);

    // Store historical data every sample_period steps
    if (t >= _next_sample_time)
    {
        _distance_samples[_current_sample++] = R;
        _next_sample_time += _sample_period;
    };

    // Every nsteps:
    //   0. Drop oldest window
    //   1. Reduce historical data for this restraint in this simulation.
    //   2. Call out to the global reduction for this window.
    //   3. On update, checkpoint the historical data source.
    //   4. Update historic windows.
    //   5. Use handles retained from previous windows to reconstruct the smoothed working histogram
    if (t >= _next_window_update_time)
    {
        // Get next histogram array, recycling old one if available.
        std::unique_ptr<PairHist> new_window{new std::vector<double>(_nbins, 0.)};
        std::unique_ptr<PairHist> temp_window;
        if (_windows.size() == _nwindows)
        {
            // Recycle the oldest window.
            // \todo wrap this in a helper class that manages a buffer we can shuffle through.
            _windows.front().swap(temp_window);
            _windows.erase(_windows.begin());
        }
        else
        {
            temp_window.reset(new std::vector<double>(_nbins));
        }

        // Reduce sampled data for this restraint in this simulation, applying a Gaussian blur to fill a grid.
        auto blur = BlurToGrid(_min_dist, _max_dist, _sigma);
        assert(new_window != nullptr);
        blur(_distance_samples, new_window.get());
        // We can just do the blur locally since there aren't many bins. Bundling these operations for
        // all restraints could give us a chance at some parallelism. We should at least use some
        // threading if we can.

        // We request a handle each time before using resources to make error handling easier if there is a failure in
        // one of the ensemble member processes and to give more freedom to how resources are managed from step to step.
        auto ensemble = _ensemble.getHandle();
        // Get global reduction (sum) and checkpoint.
        assert(temp_window != nullptr);
        ensemble.reduce(*new_window, temp_window.get());

        // Update window list with smoothed data.
        _windows.emplace_back(std::move(new_window));

        // Get new histogram difference. Subtract the experimental distribution to get the values to use in our potential.
        for (auto& bin : *_histogram)
        {
            bin = 0;
        }
        for (const auto& window : _windows)
        {
            for (auto i=0 ; i < window->size(); ++i)
            {
                (*_histogram)[i] += (*window)[i] - (*_experimental)[i];
            }
        }


        // Note we do not have the integer timestep available here. Therefore, we can't guarantee that updates occur
        // with the same number of MD steps in each interval, and the interval will effectively lose digits as the
        // simulation progresses, so _update_period should be cleanly representable in binary. When we extract this
        // to a facility, we can look for a part of the code with access to the current timestep.
        _next_window_update_time += _window_update_period;
        ++_current_window;

        // Reset sample bufering.
        _current_sample = 0;
        // Clean up drift in sample times.
        _next_sample_time = t + _sample_period;
    };

    // Compute output
    gmx::PotentialPointData output;
    // Energy not needed right now.
//    output.energy = 0;
    if (R != 0) // Direction of force is ill-defined when v == v0
    {

        double dev = R;

        double f{0};
//        if (_histogram.empty())
//        {
//            // Load from filesystem on first step.
//            _histogram = getRouxHistogram(getenv("HISTDIF"), _binWidth, _sigma, _min_dist, _max_dist);
//            assert(!_histogram.empty());
//            assert(_min_dist!=0);
//            assert(_max_dist!=0);
//        }
        if (dev > _max_dist)
        {
            f = _K * (_max_dist - dev);
        }
        else if (dev < _min_dist)
        {
            f = - _K * (_min_dist - dev);
        }
        else
        {
            double f_scal{0};

//  for (auto element : hij){
//      cout << "Hist element " << element << endl;
//    }
            size_t numBins = _histogram->size();
            //cout << "number of bins " << numBins << endl;
            double x, argExp;
            double normConst = sqrt(2 * M_PI) * pow(_sigma,
                                                    3.0);

            for (auto n = 0; n < numBins; n++)
            {
                x = n * _binWidth - dev;
                argExp = -0.5 * pow(x / _sigma,
                                    2.0);
                f_scal += _histogram->at(n) * x / normConst * exp(argExp);
            }
            f = -_K * f_scal;
        }

        output.force = f / norm(rdiff) * rdiff;
    }
    return output;
}

EnsembleRestraint::EnsembleRestraint()
{}

EnsembleResourceHandle EnsembleResources::getHandle()
{
    return {};
}

// Explicitly instantiate a definition.
template class RestraintModule<EnsembleRestraint>;

} // end namespace plugin
