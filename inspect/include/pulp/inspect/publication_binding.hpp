#pragma once

#include <pulp/inspect/capabilities.hpp>
#include <pulp/inspect/discovery.hpp>

#include <memory>
#include <vector>

namespace pulp::inspect {

/// Opaque ownership returned for one exact visible publication.
class InspectorPublicationLease {
public:
    virtual ~InspectorPublicationLease() = default;
};

/// Capability whose lifetime is scoped to one visible discovery publication.
class InspectorPublicationBinding {
public:
    virtual ~InspectorPublicationBinding() = default;
    /// Acquire one exact publication. Returns null while another publication
    /// still owns this binding. Destroying the lease conditionally releases
    /// only the publication that acquired it.
    virtual std::unique_ptr<InspectorPublicationLease> bind_publication(
        const InspectorDiscoveryRecord& record) = 0;
};

struct InspectorPublicationBindingRegistration {
    InspectorCapability capability;
    std::shared_ptr<InspectorPublicationBinding> binding;
};

/// Composition-owned source for domain publication bindings. The server uses
/// this registry to derive bindings from the same domain graph that dispatches
/// requests. New publication-scoped domains compose without server changes.
class InspectorDomainPublicationBindings {
public:
    virtual ~InspectorDomainPublicationBindings() = default;
    virtual std::vector<InspectorPublicationBindingRegistration>
    publication_bindings() const = 0;
};

} // namespace pulp::inspect
