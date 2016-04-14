// graph-tool -- a general graph modification and manipulation thingy
//
// Copyright (C) 2006-2016 Tiago de Paula Peixoto <tiago@skewed.de>
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 3
// of the License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program. If not, see <http://www.gnu.org/licenses/>.

#ifndef GRAPH_BLOCKMODEL_HH
#define GRAPH_BLOCKMODEL_HH

#include "config.h"

#include <vector>

#include "graph_state.hh"
#include "graph_blockmodel_util.hh"

namespace graph_tool
{
using namespace boost;
using namespace std;

typedef vprop_map_t<int32_t>::type vmap_t;
typedef eprop_map_t<int32_t>::type emap_t;
typedef UnityPropertyMap<int,GraphInterface::vertex_t> vcmap_t;
typedef UnityPropertyMap<int,GraphInterface::edge_t> ecmap_t;

typedef mpl::vector2<emap_t, ecmap_t> eweight_tr;
typedef mpl::vector2<vmap_t, vcmap_t> vweight_tr;
typedef mpl::vector2<std::true_type, std::false_type> use_hash_tr;
typedef mpl::vector2<simple_degs_t, degs_map_t> degs_tr;

#define BLOCK_STATE_params                                                     \
    ((g, &, all_graph_views, 1))                                               \
    ((degs,, degs_tr, 1))                                                      \
    ((eweight,, eweight_tr, 1))                                                \
    ((vweight,, vweight_tr, 1))                                                \
    ((use_hash,, use_hash_tr, 1))                                              \
    ((_abg, &, boost::any&, 0))                                                \
    ((mrs,, emap_t, 0))                                                        \
    ((mrp,, vmap_t, 0))                                                        \
    ((mrm,, vmap_t, 0))                                                        \
    ((wr,, vmap_t, 0))                                                         \
    ((b,, vmap_t, 0))                                                          \
    ((bclabel,, vmap_t, 0))                                                    \
    ((pclabel,, vmap_t, 0))                                                    \
    ((merge_map,, vmap_t, 0))                                                  \
    ((deg_corr,, bool, 0))                                                     \
    ((ignore_degrees,, typename vprop_map_t<uint8_t>::type, 0))

GEN_STATE_BASE(BlockStateBase, BLOCK_STATE_params)

template <class... Ts>
class BlockState
    : public BlockStateBase<Ts...>
{
public:
    GET_PARAMS_USING(BlockStateBase<Ts...>, BLOCK_STATE_params)
    GET_PARAMS_TYPEDEF(Ts, BLOCK_STATE_params)

    template <class RNG, class... ATs,
              typename std::enable_if_t<sizeof...(ATs) == sizeof...(Ts)>* = nullptr>
    BlockState(RNG& rng, ATs&&... args)
        : BlockStateBase<Ts...>(std::forward<ATs>(args)...),
          _bg(boost::any_cast<std::reference_wrapper<bg_t>>(__abg)),
          _c_mrs(_mrs.get_checked()),
          _emat(_g, _b, _bg, rng),
          _neighbour_sampler(get(vertex_index_t(), _g), num_vertices(_g)),
          _m_entries(num_vertices(_bg))
    {
        init_neighbour_sampler(_g, _eweight, _neighbour_sampler);
    }

    BlockState(const BlockState& other)
        : BlockStateBase<Ts...>(static_cast<const BlockStateBase<Ts...>&>(other)),
          _bg(boost::any_cast<std::reference_wrapper<bg_t>>(__abg)),
          _c_mrs(_mrs.get_checked()),
          _emat(other._emat),
          _neighbour_sampler(other._neighbour_sampler),
          _m_entries(num_vertices(_bg))
    {
        if (other.is_partition_stats_enabled())
            enable_partition_stats();
    }

    // remove a vertex from its current block
    template <class EFilt>
    void remove_vertex(size_t v, EFilt&& efilt)
    {
        typedef typename graph_traits<g_t>::vertex_descriptor vertex_t;

        vertex_t r = _b[v];

        int self_weight = 0;
        for (auto e : out_edges_range(v, _g))
        {
            if (efilt(e))
                continue;

            vertex_t u = target(e, _g);
            vertex_t s = _b[u];

            auto& me = _emat.get_bedge(e);

            auto ew = _eweight[e];
            if (u == v && !is_directed::apply<g_t>::type::value)
            {
                self_weight += ew;
            }
            else
            {
                _mrs[me] -= ew;

                assert(_mrs[me] >= 0);

                _mrp[r] -= ew;
                _mrm[s] -= ew;

                if (_mrs[me] == 0)
                    _emat.remove_me(r, s, me, _bg);
            }
        }

        if (self_weight > 0)
        {
            assert(self_weight % 2 == 0);
            const auto& me = _emat.get_me(r, r);
            _mrs[me] -= self_weight / 2;
            _mrp[r] -= self_weight / 2;
            _mrm[r] -= self_weight / 2;
            assert(_mrs[me] >= 0);
            if (_mrs[me] == 0)
                 _emat.remove_me(r, r, me, _bg);
        }

        for (auto e : in_edges_range(v, _g))
        {
            if (efilt(e))
                continue;

            vertex_t u = source(e, _g);
            if (u == v)
                continue;
            vertex_t s = _b[u];

            auto& me = _emat.get_bedge(e);

            auto ew = _eweight[e];
            _mrs[me] -= ew;

            _mrp[s] -= ew;
            _mrm[r] -= ew;

            if (_mrs[me] == 0)
                _emat.remove_me(s, r, me, _bg);
        }

        _wr[r] -= _vweight[v];

        if (!_egroups.empty())
            _egroups.remove_vertex(v, r, _g);

        if (is_partition_stats_enabled())
            get_partition_stats(v).remove_vertex(v, r, _deg_corr, _g, _vweight,
                                                 _eweight, _degs);
    }

    void remove_vertex(size_t v)
    {
        remove_vertex(v, [](auto&){ return false; });
    }

    template <class Vlist>
    void remove_vertices(Vlist& vs)
    {
        typedef typename graph_traits<g_t>::vertex_descriptor vertex_t;

        gt_hash_set<vertex_t> vset(vs.begin(), vs.end());
        typedef typename graph_traits<g_t>::edge_descriptor edges_t;

        gt_hash_set<edges_t> eset;
        for (auto v : vset)
        {
            for (auto e : all_edges_range(v, _g))
            {
                auto u = (source(e, _g) == v) ? target(e, _g) : source(e, _g);
                if (vset.find(u) != vset.end())
                    eset.insert(e);
            }
        }

        for (auto v : vset)
            remove_vertex(v, [&](auto& e) { return eset.find(e) != eset.end(); });

        for (auto& e : eset)
        {
            vertex_t v = source(e, _g);
            vertex_t u = target(e, _g);
            vertex_t r = _b[v];
            vertex_t s = _b[u];

            auto& me = _emat.get_bedge(e);

            auto ew = _eweight[e];
            _mrs[me] -= ew;

            assert(_mrs[me] >= 0);

            _mrp[r] -= ew;
            _mrm[s] -= ew;

            if (_mrs[me] == 0)
                _emat.remove_me(r, s, me, _bg);
        }
    }

    void remove_vertices(python::object ovs)
    {
        multi_array_ref<uint64_t, 1> vs = get_array<uint64_t, 1>(ovs);
        remove_vertices(vs);
    }

    // add a vertex to block r
    template <class BEdge, class Efilt>
    void add_vertex(size_t v, size_t r, BEdge&& bedge, Efilt&& efilt)
    {
        typedef typename graph_traits<g_t>::vertex_descriptor vertex_t;
        typedef typename graph_traits<bg_t>::edge_descriptor bedge_t;

        int self_weight = 0;
        for (auto e : out_edges_range(v, _g))
        {
            if (efilt(e))
                continue;
            vertex_t u = target(e, _g);
            vertex_t s;

            if (u != v)
                s = _b[u];
            else
                s = r;

            auto me = _emat.get_me(r, s);

            if (me == bedge_t())
            {
                me = add_edge(r, s, _bg).first;
                _emat.put_me(r, s, me);
                _c_mrs[me] = 0;
            }

            bedge[e] = me;

            assert(_emat.get_bedge(e) != bedge_t());
            assert(me == _emat.get_me(r, s));

            size_t ew = _eweight[e];

            if (u == v && !is_directed::apply<g_t>::type::value)
            {
                self_weight += ew;
            }
            else
            {
                _mrs[me] += ew;
                _mrp[r] += ew;
                _mrm[s] += ew;
            }
        }

        if (self_weight > 0)
        {
            assert(self_weight % 2 == 0);
            const auto& me = _emat.get_me(r, r);
            _mrs[me] += self_weight / 2;
            _mrp[r] += self_weight / 2;
            _mrm[r] += self_weight / 2;
            assert(_mrs[me] >= 0);
        }

        for (auto e : in_edges_range(v, _g))
        {
            if (efilt(e))
                continue;

            vertex_t u = source(e, _g);
            if (u == v)
                continue;

            vertex_t s = _b[u];

            auto me = _emat.get_me(s, r);

            if (me == bedge_t())
            {
                me = add_edge(s, r, _bg).first;
                _emat.put_me(s, r, me);
                _c_mrs[me] = 0;
            }

            bedge[e] = me;

            assert(_emat.get_bedge(e) != bedge_t());
            assert(me == _emat.get_me(s, r));

            size_t ew = _eweight[e];

            _mrs[me] += ew;

            _mrp[s] += ew;
            _mrm[r] += ew;
        }

        _wr[r] += _vweight[v];
        _b[v] = r;

        if (!_egroups.empty())
            _egroups.add_vertex(v, r, _eweight, _g);

        if (is_partition_stats_enabled())
            get_partition_stats(v).add_vertex(v, r, _deg_corr, _g, _vweight,
                                              _eweight, _degs);
    }

    void add_vertex(size_t v, size_t r)
    {
        add_vertex(v, r, _emat.get_bedge_map(), [](auto&) { return false; });
    }

    template <class Vlist, class Blist>
    void add_vertices(Vlist& vs, Blist& rs)
    {
        typedef typename graph_traits<g_t>::vertex_descriptor vertex_t;
        typedef typename graph_traits<bg_t>::edge_descriptor bedge_t;

        gt_hash_map<vertex_t, size_t> vset;
        for (size_t i = 0; i < vs.size(); ++i)
            vset[vs[i]] = rs[i];

        typedef typename graph_traits<g_t>::edge_descriptor edges_t;

        gt_hash_set<edges_t> eset;
        for (auto vr : vset)
        {
            auto v = vr.first;
            for (auto e : all_edges_range(v, _g))
            {
                auto u = (source(e, _g) == v) ? target(e, _g) : source(e, _g);
                if (vset.find(u) != vset.end())
                    eset.insert(e);
            }
        }

        auto bedge = _emat.get_bedge_map().get_checked();

        for (auto vr : vset)
            add_vertex(vr.first, vr.second, bedge,
                       [&](auto& e){ return eset.find(e) != eset.end(); });

        for (auto e : eset)
        {
            vertex_t v = source(e, _g);
            vertex_t u = target(e, _g);
            vertex_t r = vset[v];
            vertex_t s = vset[u];

            auto me = _emat.get_me(r, s);

            if (me == bedge_t())
            {
                me = add_edge(r, s, _bg).first;
                _emat.put_me(r, s, me);
                _c_mrs[me] = 0;
            }

            bedge[e] = me;

            assert(_emat.get_bedge(e) != bedge_t());
            assert(me == _emat.get_me(r, s));

            auto ew = _eweight[e];

            _mrs[me] += ew;
            _mrp[r] += ew;
            _mrm[s] += ew;
        }
    }

    void add_vertices(python::object ovs, python::object ors)
    {
        multi_array_ref<uint64_t, 1> vs = get_array<uint64_t, 1>(ovs);
        multi_array_ref<uint64_t, 1> rs = get_array<uint64_t, 1>(ors);
        if (vs.size() != rs.size())
            throw ValueException("vertex and group lists do not have the same size");
        add_vertices(vs, rs);
    }

    // move a vertex from its current block to block nr
    void move_vertex(size_t v, size_t nr)
    {
        size_t r = _b[v];
        if (r == nr)
            return;
        if (_bclabel[r] != _bclabel[nr])
            throw ValueException("cannot move vertex across clabel barriers");
        remove_vertex(v);
        add_vertex(v, nr);
    }

    template <class Vec>
    void move_vertices(Vec& v, Vec& nr)
    {
        for (size_t i = 0; i < std::min(v.size(), nr.size()); ++i)
            move_vertex(v[i], nr[i]);
    }

    void move_vertices(python::object ovs, python::object ors)
    {
        multi_array_ref<uint64_t, 1> vs = get_array<uint64_t, 1>(ovs);
        multi_array_ref<uint64_t, 1> rs = get_array<uint64_t, 1>(ors);
        if (vs.size() != rs.size())
            throw ValueException("vertex and group lists do not have the same size");
        move_vertices(vs, rs);
    }

    template <class VMap>
    void set_partition(VMap&& b)
    {
        for (auto v : vertices_range(_g))
            move_vertex(v, b[v]);
    }

    void set_partition(boost::any& ab)
    {
        vmap_t& b = boost::any_cast<vmap_t&>(ab);
        set_partition<typename vmap_t::unchecked_t>(b.get_unchecked());
    }

    size_t virtual_remove_size(size_t v)
    {
        return _wr[_b[v]] - _vweight[v];
    }

    // merge vertex u into v
    void merge_vertices(size_t u, size_t v)
    {
        typedef typename graph_traits<g_t>::edge_descriptor edge_t;
        UnityPropertyMap<int, edge_t> dummy;
        merge_vertices(u, v, dummy);
    }

    template <class Emap>
    void merge_vertices(size_t u, size_t v, Emap& ec)
    {
        if (u == v)
            return;
        merge_vertices(u, v, ec,
                       typename is_constant_property<vweight_t>::type(),
                       typename is_constant_property<eweight_t>::type());
    }

    template <class T1, class T2, class Emap>
    void merge_vertices(size_t, size_t, Emap&, T1, T2)
    {
        throw ValueException("cannot merge vertices of unweighted graph");
    }

    template <class Emap>
    void merge_vertices(size_t u, size_t v, Emap& ec, std::false_type,
                        std::false_type)
    {
        auto eweight_c = _eweight.get_checked();
        auto bedge_c = _emat.get_bedge_map().get_checked();

        typedef typename graph_traits<g_t>::vertex_descriptor vertex_t;
        typedef typename graph_traits<g_t>::edge_descriptor edge_t;

        gt_hash_map<std::tuple<vertex_t, int>, vector<edge_t>> ns_u, ns_v;
        for(auto e : out_edges_range(u, _g))
            ns_u[std::make_tuple(target(e, _g), ec[e])].push_back(e);
        for(auto e : out_edges_range(v, _g))
            ns_v[std::make_tuple(target(e, _g), ec[e])].push_back(e);

        for(auto& kv : ns_u)
        {
            vertex_t t = get<0>(kv.first);
            int l = get<1>(kv.first);
            auto& es = kv.second;

            size_t w = 0;
            for (auto& e : es)
                w += _eweight[e];

            if (t == u)
            {
                t = v;
                if (!is_directed::apply<g_t>::type::value)
                {
                    assert(w % 2 == 0);
                    w /= 2;
                }
            }

            auto iter = ns_v.find(std::make_tuple(t, l));
            if (iter != ns_v.end())
            {
                auto& e = iter->second.front();
                _eweight[e] += w;
            }
            else
            {
                auto e = add_edge(v, t, _g).first;
                ns_v[std::make_tuple(t, l)].push_back(e);
                eweight_c[e] = w;
                bedge_c[e] = bedge_c[es.front()];
                set_prop(ec, e, l);
            }
        }

        if (is_directed::apply<g_t>::type::value)
        {
            ns_u.clear();
            ns_v.clear();

            for(auto e : in_edges_range(v, _g))
                ns_v[std::make_tuple(source(e, _g), ec[e])].push_back(e);
            for(auto e : in_edges_range(u, _g))
                ns_u[std::make_tuple(source(e, _g), ec[e])].push_back(e);

            for(auto& kv : ns_u)
            {
                vertex_t s = get<0>(kv.first);
                int l = get<1>(kv.first);
                auto& es = kv.second;

                if (s == u)
                    continue;

                size_t w = 0;
                for (auto& e : es)
                    w += _eweight[e];

                auto iter = ns_v.find(std::make_tuple(s, l));
                if (iter != ns_v.end())
                {
                    auto& e = iter->second.front();
                    _eweight[e] += w;
                }
                else
                {
                    auto e = add_edge(s, v, _g).first;
                    ns_v[std::make_tuple(s, l)].push_back(e);
                    eweight_c[e] = w;
                    bedge_c[e] = bedge_c[es.front()];
                    set_prop(ec, e, l);
                }
            }
        }

        _vweight[v] +=_vweight[u];
        _vweight[u] = 0;
        for (auto e : all_edges_range(u, _g))
            _eweight[e] = 0;
        clear_vertex(u, _g);
        _merge_map[u] = v;
        merge_degs(u, v, _degs);
    }

    template <class EMap, class Edge, class Val>
    void set_prop(EMap& ec, Edge& e, Val& val)
    {
        ec[e] = val;
    }

    template <class Edge, class Val>
    void set_prop(UnityPropertyMap<Val, Edge>&, Edge&, Val&)
    {
    }

    void merge_degs(size_t, size_t, const simple_degs_t&) {}

    void merge_degs(size_t u, size_t v, typename degs_map_t::unchecked_t& degs)
    {
        gt_hash_map<std::tuple<size_t, size_t>, size_t> hist;
        for (auto& kn : degs[u])
            hist[make_tuple(get<0>(kn), get<1>(kn))] += get<2>(kn);
        for (auto& kn : degs[v])
            hist[make_tuple(get<0>(kn), get<1>(kn))] += get<2>(kn);
        degs[u].clear();
        degs[v].clear();
        auto& d = degs[v];
        for (auto& kn : hist)
            d.emplace_back(get<0>(kn.first), get<1>(kn.first), kn.second);
    }

    // compute the entropy difference of a virtual move of vertex from block r to nr
    template <class MEntries>
    double virtual_move_sparse(size_t v, size_t nr, MEntries& m_entries)
    {
        size_t r = _b[v];

        if (r == nr)
            return 0.;

        m_entries.clear();
        move_entries(v, nr, _b, _eweight, _mrs, _emat.get_bedge_map(), _g, _bg,
                     m_entries);

        size_t kout = out_degreeS()(v, _g, _eweight);
        size_t kin = kout;
        if (is_directed::apply<g_t>::type::value)
            kin = in_degreeS()(v, _g, _eweight);

        double dS = entries_dS(m_entries, _mrs, _emat, _bg);

        int dwr = _vweight[v];
        int dwnr = dwr;

        dS += vterm(_mrp[r]  - kout, _mrm[r]  - kin, _wr[r]  - dwr , _deg_corr, _bg);
        dS += vterm(_mrp[nr] + kout, _mrm[nr] + kin, _wr[nr] + dwnr, _deg_corr, _bg);
        dS -= vterm(_mrp[r]        , _mrm[r]       , _wr[r]        , _deg_corr, _bg);
        dS -= vterm(_mrp[nr]       , _mrm[nr]      , _wr[nr]       , _deg_corr, _bg);

        return dS;
    }

    double virtual_move_sparse(size_t v, size_t nr)
    {
        return virtual_move_sparse(v, nr, _m_entries);
    }

    template <class MEntries>
    double virtual_move_dense(size_t v, size_t nr, bool multigraph,
                              MEntries& m_entries)
    {
        if (_deg_corr)
            throw GraphException("Dense entropy for degree corrected model not implemented!");

        typedef typename graph_traits<g_t>::vertex_descriptor vertex_t;
        vertex_t r = _b[v];

        if (r == nr)
            return 0;

        // m_entries is not used in the computation below, but it is expected afterwards
        m_entries.clear();
        int kin = 0, kout = 0;
        move_entries(v, nr, _b, _eweight, _mrs, _emat.get_bedge_map(), _g, _bg,
                     m_entries);
        kout += out_degreeS()(v, _g, _eweight);
        if (is_directed::apply<g_t>::type::value)
            kin += in_degreeS()(v, _g, _eweight);

        vector<int> deltap(num_vertices(_bg), 0);
        int deltal = 0;
        for (auto e : out_edges_range(v, _g))
        {
            vertex_t u = target(e, _g);
            vertex_t s = _b[u];
            if (u == v)
                deltal += _eweight[e];
            else
                deltap[s] += _eweight[e];
        }
        if (!is_directed::apply<g_t>::type::value)
            deltal /= 2;

        vector<int> deltam(num_vertices(_bg), 0);
        for (auto e : in_edges_range(v, _g))
        {
            vertex_t u = source(e, _g);
            if (u == v)
                continue;
            vertex_t s = _b[u];
            deltam[s] += _eweight[e];
        }

        double dS = 0;
        int dwr = _vweight[v];
        int dwnr = dwr;

        double Si = 0, Sf = 0;
        for (vertex_t s = 0; s < num_vertices(_bg); ++s)
        {
            int ers = get_mrs(r, s, _mrs, _emat);
            int enrs = get_mrs(nr, s, _mrs, _emat);

            if (!is_directed::apply<g_t>::type::value)
            {
                if (s != nr && s != r)
                {
                    Si += eterm_dense(r,  s, ers,              _wr[r],         _wr[s], multigraph, _bg);
                    Sf += eterm_dense(r,  s, ers - deltap[s],  _wr[r] - dwr,   _wr[s], multigraph, _bg);
                    Si += eterm_dense(nr, s, enrs,             _wr[nr],        _wr[s], multigraph, _bg);
                    Sf += eterm_dense(nr, s, enrs + deltap[s], _wr[nr] + dwnr, _wr[s], multigraph, _bg);
                }

                if (s == r)
                {
                    Si += eterm_dense(r, r, ers,                      _wr[r],       _wr[r],       multigraph, _bg);
                    Sf += eterm_dense(r, r, ers - deltap[r] - deltal, _wr[r] - dwr, _wr[r] - dwr, multigraph, _bg);
                }

                if (s == nr)
                {
                    Si += eterm_dense(nr, nr, enrs,                       _wr[nr],        _wr[nr],        multigraph, _bg);
                    Sf += eterm_dense(nr, nr, enrs + deltap[nr] + deltal, _wr[nr] + dwnr, _wr[nr] + dwnr, multigraph, _bg);

                    Si += eterm_dense(r, nr, ers,                          _wr[r],       _wr[nr],        multigraph, _bg);
                    Sf += eterm_dense(r, nr, ers - deltap[nr] + deltap[r], _wr[r] - dwr, _wr[nr] + dwnr, multigraph, _bg);
                }
            }
            else
            {
                int esr = get_mrs(s, r, _mrs, _emat);
                int esnr = get_mrs(s, nr, _mrs, _emat);

                if (s != nr && s != r)
                {
                    Si += eterm_dense(r, s, ers            , _wr[r]      , _wr[s]      , multigraph, _bg);
                    Sf += eterm_dense(r, s, ers - deltap[s], _wr[r] - dwr, _wr[s]      , multigraph, _bg);
                    Si += eterm_dense(s, r, esr            , _wr[s]      , _wr[r]      , multigraph, _bg);
                    Sf += eterm_dense(s, r, esr - deltam[s], _wr[s]      , _wr[r] - dwr, multigraph, _bg);

                    Si += eterm_dense(nr, s, enrs            , _wr[nr]       , _wr[s]        , multigraph, _bg);
                    Sf += eterm_dense(nr, s, enrs + deltap[s], _wr[nr] + dwnr, _wr[s]        , multigraph, _bg);
                    Si += eterm_dense(s, nr, esnr            , _wr[s]        , _wr[nr]       , multigraph, _bg);
                    Sf += eterm_dense(s, nr, esnr + deltam[s], _wr[s]        , _wr[nr] + dwnr, multigraph, _bg);
                }

                if(s == r)
                {
                    Si += eterm_dense(r, r, ers                                  , _wr[r]      , _wr[r]      , multigraph, _bg);
                    Sf += eterm_dense(r, r, ers - deltap[r]  - deltam[r] - deltal, _wr[r] - dwr, _wr[r] - dwr, multigraph, _bg);

                    Si += eterm_dense(r, nr, esnr                         , _wr[r]      , _wr[nr]       , multigraph, _bg);
                    Sf += eterm_dense(r, nr, esnr - deltap[nr] + deltam[r], _wr[r] - dwr, _wr[nr] + dwnr, multigraph, _bg);
                }

                if(s == nr)
                {
                    Si += eterm_dense(nr, nr, esnr                                   , _wr[nr]       , _wr[nr]       , multigraph, _bg);
                    Sf += eterm_dense(nr, nr, esnr + deltap[nr] + deltam[nr] + deltal, _wr[nr] + dwnr, _wr[nr] + dwnr, multigraph, _bg);

                    Si += eterm_dense(nr, r, esr                         , _wr[nr]       , _wr[r]      , multigraph, _bg);
                    Sf += eterm_dense(nr, r, esr + deltap[r] - deltam[nr], _wr[nr] + dwnr, _wr[r] - dwr, multigraph, _bg);
                }
            }
        }

        return Sf - Si + dS;
    }

    double virtual_move_dense(size_t v, size_t nr, bool multigraph)
    {
        return virtual_move_dense(v, nr, multigraph, _m_entries);
    }

    template <class MEntries>
    double virtual_move(size_t v, size_t nr, bool dense, bool multigraph,
                        bool partition_dl, bool deg_dl, bool edges_dl,
                        MEntries& m_entries)
    {
        size_t r = _b[v];

        if (_bclabel[r] != _bclabel[nr])
            return std::numeric_limits<double>::infinity();

        double dS;
        if (dense)
            dS = virtual_move_dense(v, nr, multigraph, m_entries);
        else
            dS = virtual_move_sparse(v, nr, m_entries);

        if (partition_dl || deg_dl || edges_dl)
        {
            enable_partition_stats();
            auto& ps = get_partition_stats(v);
            if (partition_dl)
                dS += ps.get_delta_dl(v, r, nr, _vweight);
            if (_deg_corr && deg_dl)
                dS += ps.get_delta_deg_dl(v, r, nr, _vweight, _eweight,
                                          _degs, _g);
            if (edges_dl)
                dS += ps.get_delta_edges_dl(v, r, nr, _vweight, _g);
        }

        return dS;
    }

    double virtual_move(size_t v, size_t nr, bool dense, bool multigraph,
                        bool partition_dl, bool deg_dl, bool edges_dl)
    {
        return virtual_move(v, nr, dense, multigraph, partition_dl, deg_dl,
                            edges_dl, _m_entries);
    }

    double get_delta_dl(size_t v, size_t nr)
    {
        enable_partition_stats();
        auto& ps = get_partition_stats(v);
        return ps.get_delta_dl(v, _b[v], nr, _vweight);
    }

    // Sample node placement
    template <class RNG>
    size_t sample_block(size_t v, double c, vector<size_t>& block_list,
                        RNG& rng)
    {
        // attempt random block
        size_t s = uniform_sample(block_list, rng);

        if (!std::isinf(c) && total_degreeS()(v, _g) > 0)
        {
            auto u = sample_neighbour(_neighbour_sampler[v], rng);
            size_t t = _b[u];
            double p_rand = 0;
            if (c > 0)
            {
                size_t B = num_vertices(_bg);
                if (is_directed::apply<g_t>::type::value)
                    p_rand = c * B / double(_mrp[t] + _mrm[t] + c * B);
                else
                    p_rand = c * B / double(_mrp[t] + c * B);
            }

            typedef std::uniform_real_distribution<> rdist_t;
            if (c == 0 || rdist_t()(rng) >= p_rand)
            {
                if (_egroups.empty())
                    _egroups.init(_b, _eweight, _g, _bg);
                const auto& e = _egroups.sample_edge(t, rng);
                s = _b[target(e, _g)];
                if (s == t)
                    s = _b[source(e, _g)];
            }
        }

        return s;
    }

    size_t sample_block(size_t v, double c, vector<size_t>& block_list,
                        rng_t& rng)
    {
        return sample_block<rng_t>(v, c, block_list, rng);
    }


    template <class RNG>
    size_t random_neighbour(size_t v, RNG& rng)
    {
        if (_neighbour_sampler[v].size() == 0)
            return v;
        return sample_neighbour(_neighbour_sampler[v], rng);
    }

    // Computes the move proposal probability
    template <class MEntries>
    double get_move_prob(size_t v, size_t r, size_t s, double c, bool reverse,
                         MEntries& m_entries)
    {
        typedef typename graph_traits<g_t>::vertex_descriptor vertex_t;
        size_t B = num_vertices(_bg);
        double p = 0;
        size_t w = 0;

        size_t kout = out_degreeS()(v, _g, _eweight);
        size_t kin = kout;
        if (is_directed::apply<g_t>::type::value)
            kin = in_degreeS()(v, _g, _eweight);

        for (auto e : all_edges_range(v, _g))
        {
            vertex_t u = target(e, _g);
            if (is_directed::apply<g_t>::type::value && u == v)
                u = source(e, _g);
            vertex_t t = _b[u];
            if (u == v)
                t = r;
            size_t ew = _eweight[e];
            w += ew;

            int mts = get_mrs(t, s, _mrs, _emat);
            int mtp = _mrp[t];
            int mst = mts;
            int mtm = mtp;

            if (is_directed::apply<g_t>::type::value)
            {
                mst = get_mrs(s, t, _mrs, _emat);
                mtm = _mrm[t];
            }

            if (reverse)
            {
                int dts = m_entries.get_delta(t, s);
                int dst = dts;
                if (is_directed::apply<g_t>::type::value)
                    dst = m_entries.get_delta(s, t);

                mts += dts;
                mst += dst;

                if (t == s)
                {
                    mtp -= kout;
                    mtm -= kin;
                }

                if (t == r)
                {
                    mtp += kout;
                    mtm += kin;
                }
            }

            if (is_directed::apply<g_t>::type::value)
            {
                p += ew * ((mts + mst + c) / (mtp + mtm + c * B));
            }
            else
            {
                if (t == s)
                    mts *= 2;
                p += ew * (mts + c) / (mtp + c * B);
            }
        }
        if (w > 0)
            return p / w;
        else
            return 1. / B;
    }

    double get_move_prob(size_t v, size_t r, size_t s, double c, bool reverse)
    {
        _m_entries.clear();
        move_entries(v, (reverse) ? r : s, _b, _eweight, _mrs,
                     _emat.get_bedge_map(), _g, _bg, _m_entries);
        return get_move_prob(v, r, s, c, reverse, _m_entries);
    }

    bool is_last(size_t v)
    {
        return _wr[_b[v]] == _vweight[v];
    }

    size_t node_weight(size_t v)
    {
        return _vweight[v];
    }

    double get_deg_entropy(size_t v, const simple_degs_t&)
    {
        if (_ignore_degrees[v] == 1)
            return 0;
        auto kin = in_degreeS()(v, _g);
        auto kout = out_degreeS()(v, _g);
        if (_ignore_degrees[v] == 2)
            kout = 0;
        double S = -lgamma_fast(kin + 1) - lgamma_fast(kout + 1);
        return S * _vweight[v];
    }

    double get_deg_entropy(size_t v, typename degs_map_t::unchecked_t& degs)
    {
        if (_ignore_degrees[v] == 1)
            return 0;
        double S = 0;
        for (auto& ks : degs[v])
        {
            auto kin = get<0>(ks);
            auto kout = get<1>(ks);
            if (_ignore_degrees[v] == 2)
                kout = 0;
            int n = get<2>(ks);
            S -= n * (lgamma_fast(kin + 1) + lgamma_fast(kout + 1));
        }
        return S;
    }

    double sparse_entropy(bool multigraph, bool deg_entropy)
    {
        double S = 0;
        for (auto e : edges_range(_bg))
            S += eterm(source(e, _bg), target(e, _bg), _mrs[e], _bg);
        for (auto v : vertices_range(_bg))
            S += vterm(_mrp[v], _mrm[v], _wr[v], _deg_corr, _bg);

        if (_deg_corr && deg_entropy)
        {
            for (auto v : vertices_range(_g))
                S += get_deg_entropy(v, _degs);
        }

        if (multigraph)
        {
            for (auto v : vertices_range(_g))
            {
                gt_hash_map<decltype(v), size_t> us;
                for (auto e : out_edges_range(v, _g))
                {
                    auto u = target(e, _g);
                    if (u < v && !is_directed::apply<g_t>::type::value)
                        continue;
                    us[u] += _eweight[e];
                }

                for (auto& uc : us)
                {
                    auto& u = uc.first;
                    auto& m = uc.second;
                    if (m > 1)
                    {
                        if (u == v && !is_directed::apply<g_t>::type::value)
                        {
                            assert(m % 2 == 0);
                            S += lgamma_fast(m/2 + 1);
                        }
                        else
                        {
                            S += lgamma_fast(m + 1);
                        }
                    }
                }
            }
        }
        return S;
    }

    double dense_entropy(bool multigraph)
    {
        if (_deg_corr)
            throw GraphException("Dense entropy for degree corrected model not implemented!");
        double S = 0;
        for (auto e : edges_range(_bg))
        {
            auto r = source(e, _bg);
            auto s = target(e, _bg);
            S += eterm_dense(r, s, _mrs[e], _wr[r], _wr[s], multigraph, _bg);
        }
        return S;
    }

    double entropy(bool dense, bool multigraph, bool deg_entropy)
    {
        if (dense)
            return dense_entropy(multigraph);
        else
            return sparse_entropy(multigraph, deg_entropy);
    }

    double get_partition_dl()
    {
        enable_partition_stats();
        double S = 0;
        for (auto& ps : _partition_stats)
            S += ps.get_partition_dl();
        return S;
    }

    double get_deg_dl(bool ent, bool dl_alt, bool xi_fast)
    {
        enable_partition_stats();
        double S = 0;
        for (auto& ps : _partition_stats)
            S += ps.get_deg_dl(ent, dl_alt, xi_fast);
        return S;
    }

    template <class Vlist>
    double get_parallel_neighbours_entropy(size_t v, Vlist& us)
    {
        double S = 0;
        for (auto& uc : us)
        {
            auto& u = uc.first;
            auto& m = uc.second;
            if (m > 1)
            {
                if (u == v && !is_directed::apply<g_t>::type::value)
                {
                    assert(m % 2 == 0);
                    S += lgamma_fast(m/2 + 1);
                }
                else
                {
                    S += lgamma_fast(m + 1);
                }
            }
        }
        return S;
    }

    double get_parallel_entropy()
    {
        double S = 0;
        for (auto v : vertices_range(_g))
        {
            gt_hash_map<decltype(v), int> us;
            for (auto e : out_edges_range(v, _g))
            {
                auto u = target(e, _g);
                if (u < v && !is_directed::apply<g_t>::type::value)
                    continue;
                us[u] += _eweight[e];
            }
            S += get_parallel_neighbours_entropy(v, us);
        }
        return S;
    }

    void enable_partition_stats()
    {
        if (_partition_stats.empty())
        {
            size_t E = 0;
            for (auto e : edges_range(_g))
                E += _eweight[e];
            size_t B = 0;
            for (auto r : vertices_range(_bg))
                if (_wr[r] > 0)
                    B++;

            auto vi = std::max_element(vertices(_g).first, vertices(_g).second,
                                       [&](auto u, auto v)
                                       { return (this->_pclabel[u] <
                                                 this->_pclabel[v]); });
            size_t C = _pclabel[*vi] + 1;

            vector<vector<size_t>> vcs(C);
            vector<size_t> rc(num_vertices(_bg));
            for (auto v : vertices_range(_g))
            {
                vcs[_pclabel[v]].push_back(v);
                rc[_b[v]] = _pclabel[v];
            }

            for (size_t c = 0; c < C; ++c)
                _partition_stats.emplace_back(_g, _b, vcs[c], E, B,
                                              _vweight, _eweight, _degs,
                                              _ignore_degrees, _bmap);

            for (auto r : vertices_range(_bg))
                _partition_stats[rc[r]].get_r(r);
        }
    }

    void disable_partition_stats()
    {
        _partition_stats.clear();
    }

    bool is_partition_stats_enabled() const
    {
        return !_partition_stats.empty();
    }

    partition_stats_t& get_partition_stats(size_t v)
    {
        return _partition_stats[_pclabel[v]];
    }

    void init_mcmc(double c, double dl)
    {
        if (!std::isinf(c))
        {
            if (_egroups.empty())
                _egroups.init(_b, _eweight, _g, _bg);
        }
        else
        {
            _egroups.clear();
        }

        if (dl)
            enable_partition_stats();
        else
            disable_partition_stats();
    }


//private:
    typedef typename
        std::conditional<is_directed::apply<g_t>::type::value,
                         GraphInterface::multigraph_t,
                         UndirectedAdaptor<GraphInterface::multigraph_t>>::type
        bg_t;
    bg_t& _bg;

    typename mrs_t::checked_t _c_mrs;

    typedef typename std::conditional<use_hash_t::value,
                                      EHash<g_t, bg_t>,
                                      EMat<g_t, bg_t>>::type
        emat_t;
    emat_t _emat;

    typedef typename is_constant_property<eweight_t>::type is_weighted;
    EGroups<g_t, is_weighted> _egroups;

    typedef typename std::conditional<is_weighted::value,
                                      typename property_map_type::apply<Sampler<size_t, mpl::false_>,
                                                                        typename property_map<g_t, vertex_index_t>::type>::type,
                                      typename property_map_type::apply<vector<size_t>,
                                                                        typename property_map<g_t, vertex_index_t>::type>::type>::type::unchecked_t
        sampler_map_t;

    sampler_map_t _neighbour_sampler;
    std::vector<partition_stats_t> _partition_stats;
    std::vector<size_t> _bmap;

    EntrySet<g_t> _m_entries;
};

} // graph_tool namespace

#endif //GRAPH_BLOCKMODEL_HH
